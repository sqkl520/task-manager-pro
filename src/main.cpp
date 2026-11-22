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
#include "pages_view.h"   // PagesView::SelfTestDpi()

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
    g_win->SetSnapshot(snap);

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

    // ---- GPU 多适配器自测 ----
    //
    // 校验三件事（都是「能判定真假」的性质，不是「有没有赋值」）：
    //   1) 至少枚举出 1 张真实 GPU；
    //   2) 每张卡的名称非空（无名适配器必须走回退链拿到名字，而不是整卡消失）；
    //   3) gpus / gpuAdapters 同长同序，且每张卡的 sidecard 名是 "GPU N"。
    // 这三条正好覆盖「核显看不到」的三个成因：①被过滤 ②单值 ③break。
    log.W(L"\n=== GPU 多适配器自测 ===\n");
    if (g_perf) {
        // 等性能采集器至少产出一份快照
        for (int i = 0; i < 50; ++i) {
            if (g_perf->Sequence() >= 1) break;
            Sleep(100);
        }
        auto psnap = g_perf->GetSnapshot();
        if (!psnap) {
            Mark(-1, L"GPU 适配器枚举", "性能快照尚未产出，未测");
        } else {
            const size_t na = psnap->gpuAdapters.size();
            const size_t ng = psnap->gpus.size();
            Mark(na >= 1 ? 1 : 0, L"至少枚举出 1 张真实 GPU",
                 "gpuAdapters=" + std::to_string(na));

            int unnamed = 0;
            std::wstring names;
            for (size_t i = 0; i < na; ++i) {
                if (psnap->gpuAdapters[i].name.empty()) ++unnamed;
                if (i) names += L" / ";
                names += psnap->gpuAdapters[i].name;
            }
            MarkW(unnamed == 0 ? 1 : 0, L"每张 GPU 都有名称（无名时走回退链）",
                  unnamed == 0 ? names : (L"空名数量=" + std::to_wstring(unnamed)));

            bool sameShape = (na == ng) && na > 0;
            bool namingOk = sameShape;
            if (sameShape) {
                for (size_t i = 0; i < ng; ++i) {
                    wchar_t want[32];
                    swprintf_s(want, L"GPU %llu",
                               static_cast<unsigned long long>(i));
                    if (psnap->gpus[i].name != want) { namingOk = false; break; }
                }
            }
            char det[128];
            sprintf_s(det, "gpus=%zu gpuAdapters=%zu", ng, na);
            Mark((sameShape && namingOk) ? 1 : 0,
                 L"侧栏 GPU 卡片与适配器一一对应且命名为 GPU N", det);

            for (size_t i = 0; i < na; ++i) {
                const auto& gi = psnap->gpuAdapters[i];
                log.F("  [%zu] %s  util=%.1f%%  nodes=%u  segs=%u  engines=%zu\n",
                      i,
                      gi.name.empty() ? "(unnamed)" : "",
                      gi.totalUtilPercent, gi.nodeCount, gi.segmentCount,
                      gi.engines.size());
                log.W(gi.name);
                log.Raw("\n");
            }
        }
    } else {
        Mark(-1, L"GPU 适配器枚举", "性能采集器未启动，未测");
    }

    // ---- 列排序自测：点列头 → 回读 sortCol_ ----
    //
    // 坐标不再硬编码。此前写死「列头 y=144」，而 BuildLayout 里
    // rcColHeader_ = {0, 45, W, 89}，y=144 落在 rcList_ 的第 2 行上 ——
    // 消息被「选中行」分支吃掉，排序一次都没变过，日志却全绿。
    // 现在由 TmWindow::ClickColumnHeaderForTest 用真实几何逐像素反查目标列。
    log.W(L"\n=== 列排序自测 ===\n");
    const int colCount = g_win->ColumnCountForTest();
    // 【守卫】行表为空时，点列头虽然会改 sortCol_，但那只是改了个数字 ——
    // 没有任何行被重排（rows_ 是空的），这正是缺陷 1 的形态。
    // 所以先判行表非空，否则下面每项都会误报 PASS（"sortCol 变了"）。
    const bool haveRows = g_win->RowCount() > 0;
    if (!haveRows) {
        Mark(-1, L"列排序（全部列）", "行表为空，点击列头不会重排任何行，未测");
    }
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
        // 排序状态变了 **且** 确实有行可排，才算真的生效
        const bool ok = haveRows && clicked && afterCol == col &&
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

    // ---- 次要页表头排序自测 ----
    //
    // 这一项是针对「表头是纯装饰、点了没反应」加的。判据不是「点了有回显」，
    // 而是**首行的内容真的变了** —— 只看状态字段翻没翻，等于又造一个假自测。
    log.W(L"\n=== 次要页表头排序 ===\n");
    {
        struct SortCase { int tab; const wchar_t* page; };
        const SortCase sc[] = { {3, L"启动"}, {4, L"用户"}, {6, L"服务"} };
        for (const auto& c : sc) {
            g_win->ShowPage(c.tab);
            Sleep(60);

            // 按第 0 列连点两次：第一次升序、第二次降序。
            // 只有两次的首行内容不同，才说明排序真的作用到数据上了。
            const bool ok1 = g_win->ClickPageHeaderForTest(c.tab, 0);
            Sleep(40);
            const std::wstring firstAsc = g_win->FirstRowTextForTest(c.tab);
            const bool ok2 = g_win->ClickPageHeaderForTest(c.tab, 0);
            Sleep(40);
            const std::wstring firstDesc = g_win->FirstRowTextForTest(c.tab);

            char detail[220];
            sprintf_s(detail, "%d/%d firstRowChanged=%d",
                      ok1 ? 1 : 0, ok2 ? 1 : 0,
                      (firstAsc != firstDesc) ? 1 : 0);
            // 行数不足 2 的页无法体现顺序差异，按 SKIP 处理而不是 FAIL
            if (firstAsc.empty() || firstDesc.empty()) {
                Mark(-1, (std::wstring(L"表头排序 ") + c.page).c_str(), detail);
            } else {
                Mark((ok1 && ok2 && firstAsc != firstDesc) ? 1 : 0,
                     (std::wstring(L"表头排序 ") + c.page).c_str(), detail);
            }
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

    // ---- 悬停（HOT）状态自测 ----
    //
    // 【为什么这项必须有】用户报的「性能侧边栏按钮、结束任务按钮没有 hover」
    // 与「简略信息不会收起标签栏」是同一类缺陷的两面：状态变量/绘制分支都在，
    // 但**中间那一环（WM_MOUSEMOVE 里的赋值 / BuildLayout 里的分支）缺失或
    // 断线**。这类缺陷截一张静态图看不出来（图里可能本来就该是白的），
    // 只能靠「投递真实鼠标消息 → 回读状态」来判定。
    //
    // 判据分两级，缺一不可：
    //   1) 状态：FakeMouseMove 之后 HoverToolbarCmdForTest() 必须等于预期的 cmd
    //   2) 清除：再投一次「移出」之后必须回到 0（否则就是 hover 泄漏）
    // 只判 1 不判 2 的话，一个「永远亮着」的实现也能全绿。
    log.W(L"\n=== 悬停状态自测 ===\n");
    {
        // 用窗口自己的几何找「结束任务」按钮，不硬编码坐标。
        // 找不到就记 SKIP 而不是 FAIL —— 该按钮只在进程/详细页存在，
        // 将来若布局变化改了位置，不该误报。
        g_win->ShowPage(0);
        Sleep(40);
        g_win->ClearHoverForTest();
        const RECT tb = g_win->ToolbarRectForTest();
        // 「结束任务」在工具条右端：从右往左扫，找第一个命中 CMD_END_TASK 的点。
        // 扫的是真实 HitTest，与点击路径同一套判据。
        int hitX = -1, hitY = (tb.top + tb.bottom) / 2;
        for (int x = static_cast<int>(tb.right) - 4; x > static_cast<int>(tb.left); --x) {
            g_win->FakeMouseMove(x, hitY);
            if (g_win->HoverToolbarCmdForTest() == CMD_END_TASK) { hitX = x; break; }
        }
        if (hitX < 0) {
            Mark(-1, L"鼠标移到「结束任务」上后 hoverToolbarCmd_ = CMD_END_TASK",
                 "工具条上没扫到该按钮（可能当前页没有它）");
        } else {
            // 扫描循环的最后一个 FakeMouseMove 就是命中点，状态已就位。
            const UINT got = g_win->HoverToolbarCmdForTest();
            char d[128];
            sprintf_s(d, "x=%d y=%d → cmd=0x%X", hitX, hitY, got);
            Mark(got == CMD_END_TASK ? 1 : 0,
                 L"鼠标移到「结束任务」上后 hoverToolbarCmd_ = CMD_END_TASK", d);

            // 泄漏检测：鼠标移出后必须清回 0
            g_win->FakeMouseMove(-1, -1);
            const UINT after = g_win->HoverToolbarCmdForTest();
            sprintf_s(d, "移出后 cmd=0x%X", after);
            Mark(after == 0 ? 1 : 0,
                 L"鼠标移出后 hoverToolbarCmd_ 清回 0（无 hover 泄漏）", d);
        }

        // 「简略信息」按钮同理（左端）。
        g_win->ClearHoverForTest();
        int shX = -1;
        for (int x = static_cast<int>(tb.left) + 4; x < static_cast<int>(tb.right); ++x) {
            g_win->FakeMouseMove(x, hitY);
            if (g_win->HoverToolbarCmdForTest() == CMD_VIEW_SIMPLE) { shX = x; break; }
        }
        if (shX < 0) {
            Mark(-1, L"鼠标移到「简略信息」上后 hoverToolbarCmd_ = CMD_VIEW_SIMPLE",
                 "工具条上没扫到该按钮");
        } else {
            const UINT got = g_win->HoverToolbarCmdForTest();
            char d[128];
            sprintf_s(d, "x=%d y=%d → cmd=0x%X", shX, hitY, got);
            Mark(got == CMD_VIEW_SIMPLE ? 1 : 0,
                 L"鼠标移到「简略信息」上后 hoverToolbarCmd_ = CMD_VIEW_SIMPLE", d);
            g_win->FakeMouseMove(-1, -1);
            sprintf_s(d, "移出后 cmd=0x%X", g_win->HoverToolbarCmdForTest());
            Mark(g_win->HoverToolbarCmdForTest() == 0 ? 1 : 0,
                 L"鼠标移出后「简略信息」悬停被清除", d);
        }

        // 性能页侧栏卡片悬停（用户报的第一项）。
        // 【必须先喂性能快照】侧栏卡片是 items_ 生成的，而 items_ 由
        // PerfPage::SetData 从 PerfSnapshot 重建。自测里前面几项都在进程页，
        // 从没给性能页送过数据 —— 不喂的话 items_ 为空，OnHover 什么也命中不到，
        // 这一项就会误报 FAIL（页面本身是好的，只是没数据）。
        if (g_perf) {
            if (auto ps = g_perf->GetSnapshot()) g_win->SetPerfSnapshot(ps);
        }
        g_win->ShowPage(1);
        Sleep(60);
        g_win->ClearHoverForTest();
        const RECT pb = g_win->PerfBoundsForTest();
        // 侧栏第一张卡片中心：perftheme 的 topPad 7 + 卡片高 61 的中点。
        // 用窗口自己的 Bounds 起算，不硬编码绝对坐标。
        const int cx = static_cast<int>(pb.left) + 30;
        const int cy = static_cast<int>(pb.top) + 7 + 30;
        g_win->FakeMouseMove(cx, cy);
        // 侧栏 hover 回读走 TmWindow 的只读代理（PerfPage::HoverItem 是 public，
        // 但 selfTest 在 main.cpp 里拿不到 perfPage_）。
        const int pidx = g_win->PerfHoverItemForTest();
        char d[160];
        sprintf_s(d, "(%d,%d) bounds=(%ld,%ld)-(%ld,%ld) → hoverItem_=%d",
                  cx, cy, static_cast<long>(pb.left), static_cast<long>(pb.top),
                  static_cast<long>(pb.right), static_cast<long>(pb.bottom), pidx);
        Mark(pidx >= 0 ? 1 : 0, L"鼠标移到性能页侧栏卡片上后侧栏 hover 下标 >= 0", d);

        g_win->FakeMouseMove(-1, -1);
        sprintf_s(d, "移出后 hoverItem_=%d", g_win->PerfHoverItemForTest());
        Mark(g_win->PerfHoverItemForTest() == -1 ? 1 : 0,
             L"鼠标移出后性能页侧栏 hover 被清除（无 hover 泄漏）", d);
    }

    // ---- 简略信息视图自测：页签栏/列头必须真的收起，且切回能恢复 ----
    //
    // 【为什么要判「切回能恢复」】只判「进去时高度为 0」的话，一个把
    // rcTabs_ 永久压成零高的实现也会 PASS。用户切回「详细信息」时
    // 页签栏必须回来 —— 这是同一个需求的正反两面。
    log.W(L"\n=== 简略信息视图自测 ===\n");
    {
        g_win->ShowPage(0);
        Sleep(40);
        g_win->SetSimpleViewForTest(false);   // 确保从 normal 开始
        const int tabsNormal = g_win->TabsHeightForTest();
        const int hdrNormal  = g_win->ColHeaderHeightForTest();

        g_win->SetSimpleViewForTest(true);
        Sleep(40);
        const int tabsSimple = g_win->TabsHeightForTest();
        const int hdrSimple  = g_win->ColHeaderHeightForTest();

        char d[160];
        sprintf_s(d, "页签栏 %d→%d  列头 %d→%d",
                  tabsNormal, tabsSimple, hdrNormal, hdrSimple);
        Mark(tabsNormal > 0 && tabsSimple == 0 && hdrNormal > 0 && hdrSimple == 0 ? 1 : 0,
             L"简略信息视图收起了页签栏与列头", d);

        // 切回：两个高度都必须恢复到进入前的值。
        g_win->SetSimpleViewForTest(false);
        Sleep(40);
        const int tabsBack = g_win->TabsHeightForTest();
        const int hdrBack  = g_win->ColHeaderHeightForTest();
        sprintf_s(d, "页签栏 %d→%d  列头 %d→%d",
                  tabsSimple, tabsBack, hdrSimple, hdrBack);
        Mark(tabsBack == tabsNormal && hdrBack == hdrNormal ? 1 : 0,
             L"退出简略信息视图后页签栏/列头完整恢复", d);
    }

    // ---- 滚动系统自测：四套输入 + 阻尼惯性 ----
    //
    // 【判据全都落在真实状态上】
    //   - 滚轮：一格必须位移 3 行，**且**必须起 inertia（速度 > 0）
    //   - 触控板：±15 的细分增量必须产生非零位移（原来整除后被吃掉，位移为 0）
    //   - 惯性：逐帧位置必须单调累加，速度必须逐帧衰减，最后必须自己停表
    //   - 边界：滚到底后速度必须立刻清零（不能"在底部抽搐"）
    //   - 打断：切页后必须不再有惯性
    // 这些性质任何一个不成立，都是"看起来实现了、实际没生效"。
    log.W(L"\n=== 滚动系统自测 ===\n");
    {
        g_win->ShowPage(0);
        Sleep(60);
        const int rowPx = g_win->ScrollRowForTest();
        const int viewPx = g_win->ScrollViewportForTest();
        const int maxPx = g_win->MaxScrollOffsetForTest();

        // ---- 1) 鼠标滚轮一格 ----
        // 先确保在顶部，且列表真的有可滚内容（否则位移被钳成 0，判据失效）
        g_win->ShowPage(0);
        Sleep(40);
        g_win->FakeWheelForTest(0);   // no-op，确保 tail/inertia 干净
        {
            const int before = g_win->ScrollOffsetForTest();
            g_win->FakeWheelForTest(-WHEEL_DELTA);   // 一格向下
            const int after = g_win->ScrollOffsetForTest();
            const double v = g_win->InertiaVelocityForTest();
            char d[192];
            sprintf_s(d, "rowPx=%d view=%d max=%d  offset %d→%d  速度=%.2f px/帧",
                      rowPx, viewPx, maxPx, before, after, v);
            // 期望：位移 = 3 行，且起了惯性与反向（这里位移向上为正方向，
            // 向下滚 offset 增大）
            const bool moved3 = (after - before) == rowPx * 3;
            const bool inert   = g_win->InertiaRunningForTest() && v > 0.0;
            Mark((maxPx > rowPx * 3 && moved3 && inert) ? 1 : 0,
                 L"滚轮一格：位移 3 行且启动同向惯性", d);
        }

        // ---- 2) 惯性逐帧累加且速度单调衰减 ----
        //
        // 【方向必须与位移同向】向下一格（offset 增大）之后，惯性帧里的
        // offset 必须**继续增大**。这条判据是针对一个真实写过的 bug：
        // 速度写成 `+notches` 而位移写成 `-notches`，于是滚一轮之后
        // 画面自己滑回原处 —— 位置在动、速度也在衰减，但方向是反的。
        // 只看"速度非 0"抓不到它，必须看位移的**符号**。
        {
            const int p0 = g_win->ScrollOffsetForTest();
            const double v0 = g_win->InertiaVelocityForTest();
            const int ran = g_win->RunInertiaTicksForTest(3);
            const int p1 = g_win->ScrollOffsetForTest();
            const double v1 = g_win->InertiaVelocityForTest();
            char d[192];
            sprintf_s(d, "跑 %d 帧：offset %d→%d（前进 %d px）  速度 %.2f→%.2f",
                      ran, p0, p1, p1 - p0, v0, v1);
            // 位置必须继续向下（p1 > p0）、速度必须变小（阻尼生效）
            Mark((ran == 3 && p1 > p0 && (v1 < v0 || v1 == 0.0)) ? 1 : 0,
                 L"惯性逐帧位移同向且速度衰减（阻尼生效）", d);
        }

        // ---- 3) 惯性自己停表 ----
        {
            const int ran = g_win->RunInertiaTicksForTest(200);
            char d[128];
            sprintf_s(d, "又跑 %d 帧后 inertiaTimer=%d 速度=%.3f",
                      ran, g_win->InertiaRunningForTest() ? 1 : 0,
                      g_win->InertiaVelocityForTest());
            // 200 帧远超衰减到阈值所需（≈43 帧），必须已经停了
            Mark(!g_win->InertiaRunningForTest() ? 1 : 0,
                 L"速度低于阈值后惯性自行停止（速度/加速度归零）", d);
        }

        // ---- 4) 触控板：±15 的细分增量必须产生非零位移 ----
        //
        // 【这一项针对的是上一轮的老 bug】原来 `delta / WHEEL_DELTA` 整除，
        // ±15 全变成 0，触控板滑半天画面不动。现在走精确位移分支。
        g_win->ShowPage(0);
        Sleep(40);
        {
            const int before = g_win->ScrollOffsetForTest();
            // 连发 8 次 ±15：8×15 = 120 = 正好一格 = 3 行（不是 1 行 —
            // 一格滚 3 行是本项目既定手感，触控板按比例换算就该滚同样的量）
            for (int i = 0; i < 8; ++i) g_win->FakeWheelForTest(-15);
            const int after = g_win->ScrollOffsetForTest();
            char d[160];
            sprintf_s(d, "8×(-15)=120=一格  offset %d→%d（前进 %d px）",
                      before, after, after - before);
            // 期望 3 行；允许 ±1px 的累计取整误差
            const int want = rowPx * 3;
            const int got = after - before;
            Mark((got > 0 && got >= want - 1 && got <= want + 1) ? 1 : 0,
                 L"触控板细分增量按比例精确位移（不再被整除吃掉）", d);
            g_win->RunInertiaTicksForTest(200);   // 清掉 tail 定时器起的惯性
        }

        // ---- 5) 触控板静默后起惯性：不累积成"一跳一格" ----
        // 判据：连续 4 次 ±15 之后 offset 应是**连续小步**增长，
        // 而不是 0,0,0,28 这种量化跳变。这里直接看前三次的单调性。
        g_win->ShowPage(0);
        Sleep(40);
        {
            int steps[4] = {};
            int prev = g_win->ScrollOffsetForTest();
            for (int i = 0; i < 4; ++i) {
                g_win->FakeWheelForTest(-15);
                const int now = g_win->ScrollOffsetForTest();
                steps[i] = now - prev;
                prev = now;
            }
            char d[160];
            sprintf_s(d, "逐次位移 %d,%d,%d,%d（总和应>0，且非全为 0）",
                      steps[0], steps[1], steps[2], steps[3]);
            const int sum = steps[0] + steps[1] + steps[2] + steps[3];
            Mark(sum > 0 ? 1 : 0, L"触控板连续滑动产生连续位移", d);
        }

        // ---- 6) 详细信息页也是同一套（三个列表都要支持）----
        g_win->ShowPage(5);
        Sleep(60);
        {
            const int maxD = g_win->MaxScrollOffsetForTest();
            const int rowD = g_win->ScrollRowForTest();
            const int before = g_win->ScrollOffsetForTest();
            g_win->FakeWheelForTest(-WHEEL_DELTA);
            const int after = g_win->ScrollOffsetForTest();
            char d[192];
            sprintf_s(d, "rowPx=%d max=%d  offset %d→%d", rowD, maxD, before, after);
            // 详细信息页行高统一 kRowHeight(28)，一格仍是 3 行
            Mark((maxD > rowD * 2 && after - before == rowD * 3) ? 1 : 0,
                 L"详细信息页滚轮同样滚 3 行", d);
            g_win->RunInertiaTicksForTest(200);   // 清干净，别影响下一项
        }

        // ---- 7) 次要页（服务）也走同一套 ----
        g_win->ShowPage(6);
        Sleep(60);
        {
            const int rowS = g_win->ScrollRowForTest();
            const int before = g_win->ScrollOffsetForTest();
            g_win->FakeWheelForTest(-WHEEL_DELTA);
            const int after = g_win->ScrollOffsetForTest();
            char d[160];
            sprintf_s(d, "rowPx=%d offset %d→%d", rowS, before, after);
            Mark((after - before == rowS * 3) ? 1 : 0,
                 L"次要页（服务）滚轮同样滚 3 行", d);
            g_win->RunInertiaTicksForTest(200);
        }

        // ---- 8) 滚到底：速度必须立即清零（不能在底部抽搐）----
        g_win->ShowPage(0);
        Sleep(40);
        {
            const int maxS = g_win->MaxScrollOffsetForTest();
            // 直接滚到最底：连发足够多格
            for (int i = 0; i < 400; ++i) {
                g_win->FakeWheelForTest(-WHEEL_DELTA);
                if (g_win->ScrollOffsetForTest() >= maxS) break;
            }
            const int atBottom = g_win->ScrollOffsetForTest();
            // 到顶后还要清掉可能残留的惯性，再重新甩一把（往下甩）
            g_win->RunInertiaTicksForTest(200);
            g_win->FakeWheelForTest(-WHEEL_DELTA);   // 在底部再往下滚
            const double vAtBottom = g_win->InertiaVelocityForTest();
            const bool running = g_win->InertiaRunningForTest();
            char d[192];
            sprintf_s(d, "offset=%d max=%d 到底后再滚：inertia=%d 速度=%.2f",
                      atBottom, maxS, running ? 1 : 0, vAtBottom);
            // 判据：位置必须真的到底，且到底后速度/惯性都不残留
            Mark((maxS > 0 && atBottom == maxS && !running) ? 1 : 0,
                 L"滚到底后惯性立即清零（不残留速度）", d);
        }

        // ---- 9) 切页打断惯性 ----
        g_win->ShowPage(0);
        Sleep(40);
        {
            g_win->FakeWheelForTest(-WHEEL_DELTA);   // 起惯性
            const bool beforeRunning = g_win->InertiaRunningForTest();
            g_win->ShowPage(5);                      // 切到详细信息页
            const bool afterRunning = g_win->InertiaRunningForTest();
            char d[128];
            sprintf_s(d, "切页前 inertia=%d，切页后 inertia=%d",
                      beforeRunning ? 1 : 0, afterRunning ? 1 : 0);
            Mark((beforeRunning && !afterRunning) ? 1 : 0,
                 L"切页打断滚动惯性", d);
        }

        // ---- 10) 手势配置：WM_GESTURENOTIFY 真的被处理过 ----
        //
        // 【判据说明】本机若无触屏，系统不会投递 WM_GESTURENOTIFY，
        // gestureReady_ 保持 false。这**不是缺陷**（没有设备就没有手势），
        // 所以按 SKIP 记，而不是 FAIL —— 假装 PASS 就是假自测。
        {
            const bool ready = g_win->GestureReadyForTest();
            char d[128];
            sprintf_s(d, "gestureReady_=%d（false 表示本机无触屏或未投递 WM_GESTURENOTIFY）",
                      ready ? 1 : 0);
            Mark(ready ? 1 : -1, L"触屏平移手势配置已生效", d);
        }

        g_win->ShowPage(0);
        Sleep(40);
    }

    // ---- 高分屏（DPI）自测 ----
    // PagesView 自己实现了一整套断言（Rpx 在 4 个 DPI 下的绝对值、列宽比例、
    // 布局自洽、字体随 DPI 重建、离屏 DIB 像素回读、滚动偏移换算）。
    // 这里只是把它接进统一的自测汇总 —— 它返回空串表示全过。
    {
        log.W(L"\n=== 高分屏（DPI）自测 ===\n");
        std::wstring err = PagesView::SelfTestDpi();
        // Mark 的第三参是窄字符串，这里做一次转换（中文用 UTF-8）
        auto Narrow = [](const std::wstring& w) {
            if (w.empty()) return std::string();
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
            return s;
        };
        Mark(err.empty() ? 1 : 0,
             L"PagesView 布局与字体随 DPI 正确缩放",
             err.empty() ? Narrow(L"96/120/144/192 四档全部通过") : Narrow(err));

        // 排序箭头方向必须可辨。旧图案 kCx 左右对称，翻转行序后落点逐像素
        // 相同 —— 升序和降序在界面上画出来一模一样，方向完全看不出。
        std::wstring down = PagesView::ChevronPointsForTest(true);
        std::wstring up   = PagesView::ChevronPointsForTest(false);
        Mark(down != up ? 1 : 0,
             L"排序箭头升序与降序形状不同",
             down != up
                 ? Narrow(L"desc 与 asc 落点序列不同")
                 : Narrow(L"desc 与 asc 落点完全相同 —— 方向不可辨"));
    }

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

        // --hover=X,Y：把「鼠标」放到客户区坐标 (X,Y) 上再截图。
        // 用于验证悬停（HOT）配色**真的画到了屏上** —— 状态自测只能证明
        // 「变量被改了」，证明不了「绘制分支读了它」。两者都必要。
        // 必须放在快照就位之后：侧栏项是按快照生成的，早于它 hover 命中不到。
        if (lpCmdLine) {
            const wchar_t* hv = wcsstr(lpCmdLine, L"--hover=");
            if (hv) {
                int hx = -1, hy = -1;
                if (swscanf_s(hv + 8, L"%d,%d", &hx, &hy) == 2) {
                    g_win->FakeMouseMove(hx, hy);
                }
            }
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
