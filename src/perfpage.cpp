// perfpage.cpp - 性能页绘制实现
#include "perfpage.h"
#include "menus.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdarg>

#pragma comment(lib, "gdi32.lib")

namespace tmp {

using namespace perftheme;

namespace {

void FmtBytesGB(uint64_t b, wchar_t* out, size_t n) {
    swprintf_s(out, n, L"%.1f GB", b / 1073741824.0);
}

void FmtUptime(uint64_t sec, wchar_t* out, size_t n) {
    // 【原版是 4 段 d:hh:mm:ss】证据：docs/reference/perf_cpu_orig.png 显示
    // 「3:18:53:10」（3 天 18 小时 53 分 10 秒）。旧代码只有 3 段，
    // 运行超过 24 小时会显示「84:17:51」而不是「3:12:17:51」。
    const uint64_t d = sec / 86400;
    uint64_t rem = sec % 86400;
    const uint64_t h = rem / 3600;
    const uint64_t m = (rem % 3600) / 60;
    const uint64_t s = rem % 60;
    swprintf_s(out, n, L"%llu:%02llu:%02llu:%02llu",
               static_cast<unsigned long long>(d),
               static_cast<unsigned long long>(h),
               static_cast<unsigned long long>(m),
               static_cast<unsigned long long>(s));
}

// 性能页 Sidebar 的热力图着色（比进程页更饱和，因为要区分 20 个格子）
COLORREF CoreHeat(double util) {
    if (util < 0) util = 0;
    if (util > 100) util = 100;
    double t = util / 100.0;
    // 从 #F0F0F0 插值到 #0078D7
    int r = static_cast<int>(0xF0 - (0xF0 - 0x00) * t + 0.5);
    int g = static_cast<int>(0xF0 - (0xF0 - 0x78) * t + 0.5);
    int b = static_cast<int>(0xF0 - (0xF0 - 0xD7) * t + 0.5);
    return RGB(r, g, b);
}

} // namespace

void PerfHistory::Push(double c, double m, double d, double n, double g) {
    cpu[head] = c; mem[head] = m; disk[head] = d; net[head] = n; gpu[head] = g;
    // cpuKernel 由 SetData 紧跟本调用之后单独写入同一个槽位（它不在本函数的
    // 参数里，因为只有 CPU 页需要它，加参数会让另外 4 个调用点全部改签名）。
    // 逐实例的 diskRateI / netSentI / netRecvI / gpuI 同理。
    head = (head + 1) % kPoints;
    if (count < kPoints) ++count;
}

// 逐核历史：和主曲线共用同一个 head/count 游标，保证时间轴对齐。
// 注意调用顺序 —— 必须和 Push() 配套，Push 已经把 head 前移了，
// 所以这里要写到前一个槽位（head-1）。
void PerfHistory::PushCores(const std::vector<CpuCoreSample>* cores) {
    if (!cores) return;
    int n = static_cast<int>(cores->size());
    if (n > kMaxCores) n = kMaxCores;
    coreCount = n;

    int slot = (head - 1 + kPoints) % kPoints;   // Push() 刚推进过的那个槽
    for (int i = 0; i < n; ++i) {
        double v = (*cores)[i].utilPercent;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        coreHist[i][slot] = v;
    }
    // 核心数变少时（几乎不会发生）把多余的行清零，免得画出旧数据
    for (int i = n; i < kMaxCores; ++i) coreHist[i][slot] = 0.0;
}

PerfPage::~PerfPage() {
    auto del = [](HFONT& f) { if (f) { DeleteObject(f); f = nullptr; } };
    del(fontTitle_); del(fontSub_); del(fontBody_); del(fontBold_);
    del(fontBig_); del(fontSmall_); del(fontTiny_); del(fontBody2_); del(fontValue_);
    del(fontStat_); del(fontSide1_); del(fontSide2_); del(fontSide3_);

    // 画笔/画刷缓存必须在这里释放，否则就是真泄漏（每个缓存对象活到进程退出）。
    // 缓存表上界是「本页用到的常量色个数」，实际只有十几个，不是无界容器。
    for (auto& kv : penCache_)   if (kv.second) DeleteObject(kv.second);
    for (auto& kv : brushCache_) if (kv.second) DeleteObject(kv.second);
    penCache_.clear();
    brushCache_.clear();
}

void PerfPage::BuildFonts() {
    auto mk = [](int px, int w) -> HFONT {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        LOGFONTW lf{};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            lf = ncm.lfMessageFont;
        } else {
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
        }
        lf.lfHeight = -px;
        lf.lfWeight = w;
        lf.lfQuality = CLEARTYPE_QUALITY;
        return CreateFontIndirectW(&lf);
    };
    // 字号依据（主题 GetThemeFont 实测，U_themecolors.md §2.7 / S_duispec.md §2.3）：
    //   【主题实测 GetThemeFont】TASKMANAGER part 14 = YaHei UI -29px/400 → 页标题 resourceTitle
    //   【主题实测 GetThemeFont】TASKMANAGER part 15 = -16px/400 → 处理器名 resourceName
    //   【主题实测 GetThemeFont】TASKMANAGER part 17 = -12px/400 → staticLabel/sidebarData
    //   【主题实测 GetThemeFont】TASKMANAGER part 19 = -21px/400 → dynamicData（统计数值）
    //   【主题实测 GetThemeFont】TEXTSTYLE    4,0  = -12px/400 → 全局正文（列头/内容/页签）
    //   【UIFILE 未定义】13/17/22/11/10 这几档没有对应 gtf 引用，保留旧值。
    fontTitle_ = mk(28, FW_NORMAL);      // 【无依据，沿用旧值】主题 part 14 是 29px，差 1
    fontSub_   = mk(20, FW_NORMAL);      // 【无依据，沿用旧值】主题 part 15 是 16px，差 4
    fontBody_  = mk(12, FW_NORMAL);      // 【主题实测 GetThemeFont】part 17 / TEXTSTYLE 4,0 = 12px
    fontBody2_ = mk(13, FW_NORMAL);      // 【UIFILE 未定义，沿用旧值】
    fontBold_  = mk(12, FW_SEMIBOLD);    // 【UIFILE 未定义，沿用旧值】主题全部字体 weight 均为 400
    fontValue_ = mk(17, FW_NORMAL);      // 【UIFILE 未定义，沿用旧值】
    fontStat_  = mk(21, FW_NORMAL);      // 【主题实测 GetThemeFont】part 19 = -21px/400，不是粗体
    fontBig_   = mk(22, FW_NORMAL);      // 【UIFILE 未定义，沿用旧值】
    fontSmall_ = mk(11, FW_NORMAL);      // 【主题实测 GetThemeFont】part 44 lheader/rheader = -11px
    fontTiny_  = mk(10, FW_NORMAL);      // 【UIFILE 未定义，沿用旧值】
    // 侧栏三档字号：
    // 【主题实测 GetThemeFont】sidebarLabel → gtf(TASKMANAGER,16,0) = 16px/400
    // 【主题实测 GetThemeFont】sidebarData  → gtf(TASKMANAGER,17,0) = 12px/400
    // 【UIFILE 未定义】第三档 11px：侧栏没有第三个 gtf 引用（sidebarDataHeader 也用 part 17 = 12px）。
    fontSide1_ = mk(16, FW_NORMAL);      // 侧栏卡片第 1 行（名称）—— part 16 = 16px
    fontSide2_ = mk(12, FW_NORMAL);      // 侧栏卡片第 2 行（数值）—— part 17 = 12px
    fontSide3_ = mk(11, FW_NORMAL);      // 侧栏卡片第 3 行（明细）—— 【UIFILE 未定义，沿用旧值】
}

void PerfPage::SetChart(ChartMode m) {
    chart_ = m;
    // 【P0-6 关键】把「将图形更改为」真正接上视图，而不是只改一个没人读的开关。
    //
    // 原生 Task Manager 的语义就是「换图形」：选「逻辑处理器」你看到逐核格子，
    // 选「多个引擎」你看到 GPU 的引擎网格 —— 它从不要求用户先自己切页。
    // 以前这里只赋值，绘制侧只有 Overall/Logical 两个分支，于是：
    //   点「NUMA 节点」/「单个引擎」/「多个引擎」→ 圆点动了，画面纹丝不动。
    // 现在按模式把页面绑到能画出它的那一页：
    //   Logical / Numa        → CPU 页（逐核网格 / 逐 NUMA 节点网格）
    //   SingleEngine/MultiEng → GPU 页（放大单引擎 / 满屏引擎网格）
    //   Overall               → 不动当前页（CPU 与 GPU 页都有「总体利用率」语义）
    switch (m) {
    case ChartMode::Logical:
    case ChartMode::Numa:
        SetView(PerfView::Cpu);
        break;
    case ChartMode::SingleEngine:
    case ChartMode::MultiEngine:
        SetView(PerfView::Gpu);
        break;
    case ChartMode::Overall:
    default:
        break;   // 保持当前页
    }
}

void PerfPage::SetView(PerfView v) {
    view_ = v;
    itemsDirty_ = true;
    SyncSelectionToView();
}

void PerfPage::SetViewByName(const wchar_t* name) {
    if (!name) return;
    if (_wcsicmp(name, L"cpu") == 0)          { SetView(PerfView::Cpu); }
    else if (_wcsicmp(name, L"memory") == 0)  { SetView(PerfView::Memory); }
    else if (_wcsicmp(name, L"disk") == 0)    { SetView(PerfView::Disk); }
    else if (_wcsicmp(name, L"network") == 0) { SetView(PerfView::Network); }
    else if (_wcsicmp(name, L"gpu") == 0)     { SetView(PerfView::Gpu); }
}

// 【P0-7 一致性】正文页与侧栏高亮必须指向同一张卡片。
// 菜单「查看▸」切页、--view= 启动参数、图表上方按钮切页都会绕过 OnClick，
// 那些路径下 selectedItem_ 仍停在旧卡片上，屏幕上就会出现
// 「高亮磁盘 0、正文是网络页」这种自相矛盾的状态。
// 这里把选中项挪到「该视图当前选中的那张卡片」（该方法视图还没选过就取实例 0）。
void PerfPage::SyncSelectionToView() {
    const int vi = static_cast<int>(view_);
    if (view_ == PerfView::Cpu || view_ == PerfView::Memory) {
        // 没有实例概念的页：选中项是唯一的那张卡片
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].view == view_) {
                selectedItem_ = static_cast<int>(i);
                return;
            }
        }
        selectedItem_ = 0;
        return;
    }
    // 磁盘/网络/GPU：高亮到「该视图当前选中的那张卡片」。
    // 该视图还没有选中过时（selInst_ = -1），按实例 0 算 —— ClampInstance
    // 在绘制侧也会把 -1 夹成 0，两边一致。
    const int want = (selInst_[vi] >= 0) ? selInst_[vi] : 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].view == view_ && items_[i].index == want) {
            selectedItem_ = static_cast<int>(i);
            return;
        }
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].view == view_) {
            selectedItem_ = static_cast<int>(i);
            return;
        }
    }
    // items_ 还没生成（首帧之前）：高亮留给首帧的 OnClick/绘制决定
}

void PerfPage::SetData(std::shared_ptr<const PerfSnapshot> snap) {
    if (!snap) return;
    snap_ = std::move(snap);

    hist_.Push(snap_->cpuTotalPercent,
               snap_->mem.totalBytes ?
                   static_cast<double>(snap_->mem.inUseBytes) /
                   static_cast<double>(snap_->mem.totalBytes) * 100.0 : 0.0,
               // 磁盘：把 MB/s 映射到 0-100 的图形刻度（500 MB/s 视作满格）
               [&] {
                   const auto& ds = snap_->disks;
                   double mbps = ds.empty() ? 0.0 : ds[0].readMBps + ds[0].writeMBps;
                   double t = mbps / 500.0 * 100.0;
                   return t > 100.0 ? 100.0 : t;
               }(),
               // 网络：同理，按 1 Gbps 满格映射
               [&] {
                   double mbps = 0.0;
                   for (const auto& n : snap_->nets) {
                       double m = (std::max)(n.recvMbps, n.sendMbps);
                       if (m > mbps) mbps = m;
                   }
                   double t = mbps / 1000.0 * 100.0;
                   return t > 100.0 ? 100.0 : t;
               }(),
               snap_->gpuTotalPercent);
    // 逐核历史必须紧跟 Push：Push 内部推进了 head，PushCores 写的是前一槽位
    hist_.PushCores(&snap_->cores);

    // 内核时间历史：与 Push 写的是同一条时间轴，取它刚推进过的那个槽位。
    {
        const int slot = (hist_.head - 1 + PerfHistory::kPoints) % PerfHistory::kPoints;
        double kp = snap_->cpuKernelPercent;
        if (kp < 0) kp = 0;
        if (kp > 100) kp = 100;
        hist_.cpuKernel[slot] = kp;
    }

    // ---- 逐实例（磁盘 / 网络 / GPU）历史 ----
    // 【P0-7】以前这里只记一条「全系统聚合」曲线：
    //   磁盘固定取 disks[0]，网络取「最忙的那个适配器」，GPU 只有一条总利用率。
    //   于是侧栏点哪张卡片都只改高亮，图表数字永远是第一个。
    // 现在按实例下标分别记，绘制侧按 selInst_[view_]（经 ClampInstance）取对应那一份。
    // 上限 PerfHistory::kMaxInstances，与侧栏卡片数上限一致。
    {
        const int slot = (hist_.head - 1 + PerfHistory::kPoints) % PerfHistory::kPoints;

        const int nd = (std::min)(static_cast<int>(snap_->disks.size()),
                                  PerfHistory::kMaxInstances);
        hist_.diskCount = nd;
        for (int i = 0; i < nd; ++i) {
            const auto& d = snap_->disks[i];
            double pct = d.activePercent;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            hist_.diskActiveI[i][slot] = pct;
            hist_.diskRateI[i][slot] = d.readMBps + d.writeMBps;
        }

        const int nn = (std::min)(static_cast<int>(snap_->nets.size()),
                                  PerfHistory::kMaxInstances);
        hist_.netCount = nn;
        for (int i = 0; i < nn; ++i) {
            const auto& n = snap_->nets[i];
            hist_.netSentI[i][slot] = n.sendMbps;
            hist_.netRecvI[i][slot] = n.recvMbps;
        }

        const int ng = (std::min)(static_cast<int>(snap_->gpus.size()),
                                  PerfHistory::kMaxInstances);
        hist_.gpuCount = ng;
        for (int i = 0; i < ng; ++i) {
            hist_.gpuI[i][slot] = snap_->gpus[i].utilPercent;
        }

        // 已提交字节（Win10 内存页第二条曲线画的是这个）
        hist_.memCommit[slot] = static_cast<double>(snap_->mem.committedBytes);

        // GPU 各引擎历史。槽位顺序固定为 3D / Copy / Video Encode / Video Decode，
        // 和 PaintGpuPage 里的 wanted[] 一一对应。
        // 【多卡】同时记两份：
        //   gpuEngine  = 所有卡的合并视图（每种引擎类型取最忙的那张卡），
        //                给"没有实例概念"的绘制路径兜底；
        //   gpuEngineI = 逐卡明细，选中 GPU 1 时画的就是 GPU 1 自己的引擎曲线。
        const int wantedTypes[PerfHistory::kEngineSlots] = { 1, 6, 3, 2 };
        for (int e = 0; e < PerfHistory::kEngineSlots; ++e) {
            double v = 0.0;
            for (const auto& eng : snap_->gpuEngines) {
                if (eng.engineType == wantedTypes[e]) { v = eng.utilPercent; break; }
            }
            hist_.gpuEngine[e][slot] = v;
        }
        for (int i = 0; i < ng; ++i) {
            // gpuAdapters 与 gpus 同下标；取不到就整行清零（不画别张卡的数据）
            const std::vector<GpuEngineSample>* src = nullptr;
            if (i < static_cast<int>(snap_->gpuAdapters.size()))
                src = &snap_->gpuAdapters[i].engines;
            for (int e = 0; e < PerfHistory::kEngineSlots; ++e) {
                double v = 0.0;
                if (src) {
                    for (const auto& eng : *src) {
                        if (eng.engineType == wantedTypes[e]) { v = eng.utilPercent; break; }
                    }
                }
                hist_.gpuEngineI[i][e][slot] = v;
            }
        }
    }

    itemsDirty_ = true;
}

void PerfPage::EnsureSidebarItems() {
    if (itemsDirty_) RebuildSidebarItems();   // RebuildSidebarItems 内部清 itemsDirty_
}

void PerfPage::RebuildSidebarItems() {
    items_.clear();
    // 无论有没有快照，重建过就是干净的：置 false 必须在这里做，
    // 不能只放在调用点（否则一条 return 早退就会让 itemsDirty_ 永远为真，
    // 每帧都重建一次侧栏项）。
    itemsDirty_ = false;
    if (!snap_) return;

    // 每张卡片 = 1 张迷你图表（sheet=chart，60rp×45rp）+ 若干行文字。
    // 【主题实测 GetThemeColor】迷你图与主图曲线色按类别区分：
    //   CPU/GPU 蓝 #117DBB（CHARTVIEW 2/4）、内存紫 #8B12AE（9）、
    //   磁盘绿 #4DA60C（16）、网络棕橙 #A74F01（23）。

    auto W = [](const wchar_t* fmt, ...) -> std::wstring {
        wchar_t b[192];
        va_list ap; va_start(ap, fmt);
        _vsnwprintf_s(b, _TRUNCATE, fmt, ap);
        va_end(ap);
        return b;
    };

    // ---- CPU（2 行）----
    {
        double mhz = snap_->cpuCurrentMhz / 1000.0;
        items_.push_back({
            L"CPU",
            W(L"%.0f%%  %.2f GHz", snap_->cpuTotalPercent, mhz),
            L"",
            PerfView::Cpu, 0,
            kChartLineCpu                  // 【主题实测 GetThemeColor】CHARTVIEW part 2 FILL = #117DBB
        });
    }

    // ---- 内存（2 行）----
    {
        double usedGB = snap_->mem.inUseBytes / 1073741824.0;
        double totalGB = snap_->mem.totalBytes / 1073741824.0;
        double pct = snap_->mem.totalBytes
            ? static_cast<double>(snap_->mem.inUseBytes) /
              static_cast<double>(snap_->mem.totalBytes) * 100.0 : 0.0;
        items_.push_back({
            L"内存",
            W(L"%.1f/%.1f GB (%.0f%%)", usedGB, totalGB, pct),
            L"",
            PerfView::Memory, 0,
            kChartLineMem                  // 【主题实测 GetThemeColor】CHARTVIEW part 9 FILL = #8B12AE
        });
    }

    // ---- 磁盘（2 行）----
    // 结构依据是 **UIFILE 元素树**（docs/specs/S_duispec.md §3.2，dashSidebarDiskButton）：
    //   dashSidebarDiskChart   sheet=chart（60rp×45rp）
    //   sidebar_disk_name / sidebar_disk_type / sidebar_disk_util / sidebar_disk_server /
    //   sidebar_disk_r「读取」+ sidebar_disk_read / sidebar_disk_w「写入」+ sidebar_disk_write
    // 也就是说原版磁盘卡**文字元素比 CPU/内存卡多**（含介质类型与读写明细）。
    // 我方 SidebarItem 只有 name/sub/detail 三槽，且 perfdata 的 DiskSample 没有介质类型字段，
    // 所以这里保持 2 行（name + 读写合计）—— 这是**字段缺失**造成的，不是几何结论。
    // 单位「MB/s」来自 MUI 32881 的说明，本机显示即 MB/s。
    // Win10 磁盘卡是 3 行（docs/reference 已验真）：
    //   第 1 行 `磁盘 0 (C: D:)`  —— 名称 + 盘符列表
    //   第 2 行 `SSD`             —— 介质类型（**不含总线**；`(NVMe)` 是 Win11 形态）
    //   第 3 行 `读取: 0 KB/秒 写入: 0 …`
    // 首轮没有基线时读写不可定义，必须显示「—」而不是 0 ——
    // 显示 0 等于谎报「磁盘刚才完全空闲」。
    for (size_t i = 0; i < snap_->disks.size() && i < 8; ++i) {
        const auto& d = snap_->disks[i];

        std::wstring title = d.name;
        if (!d.driveLetters.empty()) {
            title += L" ";
            title += d.driveLetters;
        }

        // 介质类型优先；未知时留空（原版无数据时这一行是空的）
        std::wstring sub = d.mediaType;

        wchar_t detail[128];
        if (d.hasBaseline) {
            swprintf_s(detail, L"读取: %.1f MB/秒 写入: %.1f MB/秒",
                       d.readMBps, d.writeMBps);
        } else {
            wcscpy_s(detail, L"读取: — 写入: —");
        }

        items_.push_back({
            title,
            sub,
            detail,
            PerfView::Disk, static_cast<int>(i),
            kChartLineDisk                 // 【主题实测 GetThemeColor】CHARTVIEW part 16 FILL = #4DA60C
        });
    }
    if (snap_->disks.empty()) {
        // index = -1：占位卡片没有对应实例，绘制侧不会去索引 disks[0]
        items_.push_back({ L"磁盘", L"—", L"", PerfView::Disk, -1, kChartLineDisk });
    }

    // ---- 网络（3 行）----
    // Win10 原版的三行是：适配器名 / 连接类型 / 收发速率。
    // 旧代码把第二行硬编码成 L"以太网"，于是 Wi-Fi 卡也显示「以太网」——
    // 这是直接说谎（docs/reference/perf_cpu_orig.png 的 Wi-Fi 卡第二行不是它）。
    for (size_t i = 0; i < snap_->nets.size() && i < 4; ++i) {
        const auto& n = snap_->nets[i];
        // 连接类型：从适配器名推断。含 "Wi" / "WLAN" / "Wireless" 判为 Wi-Fi。
        std::wstring connType;
        const std::wstring& nm = n.name;
        if (nm.find(L"Wi-Fi") != std::wstring::npos ||
            nm.find(L"WLAN")  != std::wstring::npos ||
            nm.find(L"Wireless") != std::wstring::npos ||
            nm.find(L"802.11") != std::wstring::npos) {
            connType = L"Wi-Fi";
        } else if (nm.find(L"VMware") != std::wstring::npos ||
                   nm.find(L"Virtual") != std::wstring::npos ||
                   nm.find(L"Hyper-V") != std::wstring::npos) {
            connType = L"以太网";
        } else if (n.hasName) {
            connType = L"以太网";
        } else {
            connType = L"—";
        }
        items_.push_back({
            n.name,
            connType,
            W(L"发送: %.1f Kbps  接收: %.1f Kbps",
              n.sendMbps * 1000.0, n.recvMbps * 1000.0),
            PerfView::Network, static_cast<int>(i),
            kChartLineNet                  // 【主题实测 GetThemeColor】CHARTVIEW part 23 FILL = #A74F01
        });
    }
    if (snap_->nets.empty()) {
        // index = -1：占位卡片没有对应实例
        items_.push_back({ L"以太网", L"—", L"—", PerfView::Network, -1,
                           kChartLineNet });
    }

    // ---- GPU（多卡：每张真实 GPU 一张卡片）----
    //
    // 【UIFILE 声明】dashSidebarGpuEntry 的文字元素比 CPU/内存卡多：
    //   sidebar_gpu_name / adapter / util / temp（S_duispec.md §3.2 元素树）
    // 也就是说「GPU 0」和「适配器名」在原版里是**两个独立元素**，不是拼成
    // 一行 "GPU 0 - NVIDIA ..."。所以这里：
    //   第 1 行 name   = "GPU N"（MUI 实测 `GPU %d`）
    //   第 2 行 sub    = "12%  3.0 GB"（利用率 + 专用显存）
    //   第 3 行 detail = 适配器名（对应原版的 adapter 元素；temp 我们没有数据，不编造）
    // 卡片高由 hasLine3 自动变成 kSidebarCardH3，只影响 GPU 卡。
    for (size_t i = 0; i < snap_->gpus.size() && i < 4; ++i) {
        const auto& g = snap_->gpus[i];
        wchar_t gb[32];
        _snwprintf_s(gb, _TRUNCATE, L"%.1f GB", g.dedicatedBytes / 1073741824.0);

        // 适配器名取自同下标的 gpuAdapters（与 gpus 同源同序）
        const wchar_t* adapterName = L"";
        if (i < snap_->gpuAdapters.size()) adapterName = snap_->gpuAdapters[i].name.c_str();

        items_.push_back({
            g.name,
            W(L"%.0f%%  %s", g.utilPercent, gb),
            adapterName,
            PerfView::Gpu, static_cast<int>(i),
            kChartLineGpu                  // 【主题实测 GetThemeColor】同 CPU：CHARTVIEW part 2 FILL = #117DBB
        });
    }
    if (snap_->gpus.empty()) {
        // index = -1：占位卡片没有对应实例
        items_.push_back({ L"GPU", L"—", L"", PerfView::Gpu, -1, kChartLineGpu });
    }

    // 卡片清单重建后，高亮下标可能指向旧清单的位置（实例数/卡片数变了）。
    // 这里按当前 view_ 重新对齐一次，避免出现「高亮落在被 break 掉的卡片上」
    // 或者「当前是磁盘页、高亮却在 CPU 卡上」。
    SyncSelectionToView();
}

// 卡片左侧的迷你历史图表。
// 【UIFILE 声明】尺寸来自 `chart` 样式（docs/evidence/exec/_S_geo2_30001.tsv @0x284..@0x290）：
//   width = 60rp、height = 45rp、padding = rect(0rp,5rp,0rp,0rp)、margin = rect(20rp,10rp,10rp,10rp)
//   元素 = dashSidebar{Cpu,Memory,Disk,Network,Gpu}Chart，sheet="chart"。
// 边框/曲线/填充的**颜色**主题与 UIFILE 都取不到（CHARTV.dll 内部绘制），
// 现用的「1px 同色边框 + 1px 曲线 + 6.7% 淡化填充」是既有视觉实现，非原版证据。
void PerfPage::PaintSidebarMiniChart(HDC hdc, const RECT& rc,
                                     const SidebarItem& it) const {
    Fill(hdc, rc, RGB(0xFF, 0xFF, 0xFF));
    // ↑ 【UIFILE 引用系统色】chart 样式 background = window；本机
    //   GetSysColor(COLOR_WINDOW) = #FFFFFF（R_sysmetrics.md §5.1），与旧值一致。

    // 1px 边框用**该卡片自己的强调色**（不是灰色常量）。
    // 【无依据，沿用旧值】边框着色规则来自已删除的「逐像素反解」（k=0.36/0.64/0.48/0.50），
    // 那批系数是拿我方旧截图算的；这里只保留「同色 1px」这个视觉结论。
    {
        HGDIOBJ op = SelectObject(hdc, GetStockObject(DC_PEN));
        SetDCPenColor(hdc, it.chartColor);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
    }

    // 选哪条历史曲线。
    // 【P0-7】磁盘/网络/GPU 卡片要画**这张卡片自己那个实例**的历史，
    //   不能用聚合曲线（聚合 = disks[0] / 最忙网卡）。
    //   it.index < 0 是无实例的占位卡片，回退到聚合曲线（全 0，不会说谎）。
    const double* src = hist_.cpu;
    std::vector<double> tmp;   // 需要逐点换算时（网络卡）用的临时缓冲
    switch (it.view) {
    case PerfView::Cpu:
        src = hist_.cpu;
        break;
    case PerfView::Memory:
        src = hist_.mem;
        break;
    case PerfView::Disk:
        if (it.index >= 0 && it.index < hist_.diskCount) {
            // 逐盘「活动时间 %」，与主图的量纲一致（0..100）
            src = hist_.diskActiveI[it.index];
        } else {
            src = hist_.disk;
        }
        break;
    case PerfView::Network: {
        if (it.index >= 0 && it.index < hist_.netCount) {
            // 收发各是一条 MB/s 曲线，卡片只有一格，取逐点最大值后按
            // 「该实例历史峰值」归一到 0..100 —— 否则 1 Mbps 的网卡会是一条贴底直线。
            const double* s = hist_.netSentI[it.index];
            const double* r = hist_.netRecvI[it.index];
            double mx = 0.0;
            for (int k = 0; k < PerfHistory::kPoints; ++k) {
                double v = (std::max)(s[k], r[k]);
                if (v > mx) mx = v;
            }
            if (mx <= 0.0) mx = 1.0;
            // ⚠ tmp 必须**按环形槽位**填，不能按时间顺序填：
            //   下面绘图用的是 hist_.At(src, i)，而 At() 内部会对下标做
            //   (head - count + i) % kPoints 的环形换算。如果这里按时间顺序存，
            //   曲线会被再转一次，画出来是错位的锯齿。
            tmp.assign(PerfHistory::kPoints, 0.0);
            for (int k = 0; k < PerfHistory::kPoints; ++k) {
                const int ring = (hist_.head - hist_.count + k
                                  + PerfHistory::kPoints * 2) % PerfHistory::kPoints;
                double v = (std::max)(s[ring], r[ring]) / mx * 100.0;
                tmp[ring] = v > 100.0 ? 100.0 : v;
            }
            src = tmp.data();
        } else {
            src = hist_.net;
        }
        break;
    }
    case PerfView::Gpu:
        if (it.index >= 0 && it.index < hist_.gpuCount) {
            src = hist_.gpuI[it.index];
        } else {
            src = hist_.gpu;
        }
        break;
    default:
        break;
    }
    if (hist_.count < 2) return;

    RECT plot{ rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
    int pw = plot.right - plot.left;
    int ph = plot.bottom - plot.top;
    if (pw < 4 || ph < 4) return;

    const int n = hist_.count;
    std::vector<POINT> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        double v = hist_.At(src, i);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        int px = plot.left + (n > 1 ? i * (pw - 1) / (n - 1) : 0);
        int py = plot.bottom - static_cast<int>(v / 100.0 * (ph - 1) + 0.5);
        if (py < plot.top) py = plot.top;
        pts.push_back(POINT{ px, py });
    }

    if (pts.size() >= 2) {
        // 填充：约 6.7% alpha 的同色淡化
        // 【无依据，沿用旧值】CHARTV.dll 的填充算法 UIFILE 与主题都不描述，
        // 6.7% 是既有实现值，原参考图已证伪，没有可替换的权威来源。
        std::vector<POINT> poly = pts;
        poly.push_back(POINT{ pts.back().x, plot.bottom });
        poly.push_back(POINT{ pts.front().x, plot.bottom });

        int r = GetRValue(it.chartColor);
        int g = GetGValue(it.chartColor);
        int b = GetBValue(it.chartColor);
        // k 是「朝白色混合的比例」，0.067 = 混 6.7%。这是小数，lambda 参数必须是
        // double —— 之前写成 int 会让 0.067 被截成 0，mix 退化成恒等函数，
        // 填充色变成纯白（C4244 警告就是在提示这个）。
        auto mix = [](int v, double k) {
            return static_cast<int>(255.0 - (255.0 - v) * k + 0.5);
        };
        // 【性能】改用缓存对象：侧栏 5 张卡片 × 每次重绘，原来每张建 2 个对象。
        // 填充色是按 chartColor 插值算出来的，但 chartColor 本身来自
        // RebuildSidebarItems 里写入的常量（每类图表固定一色），所以
        // 插值结果也是有限个值，缓存不会无限增长。
        HBRUSH fb = CachedBrush(RGB(mix(r, 0.067), mix(g, 0.067), mix(b, 0.067)));
        HGDIOBJ ob2 = SelectObject(hdc, fb);
        HGDIOBJ op2 = SelectObject(hdc, GetStockObject(NULL_PEN));
        Polygon(hdc, poly.data(), static_cast<int>(poly.size()));
        SelectObject(hdc, ob2);
        SelectObject(hdc, op2);

        // 1px 曲线
        HPEN pen = CachedPen(it.chartColor);
        HGDIOBJ oldp = SelectObject(hdc, pen);
        Polyline(hdc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(hdc, oldp);
    }
}

// ---- 画笔 / 画刷缓存 ----
// 见 include/perfpage.h 里 penCache_/brushCache_ 的注释。
// 这两个函数取代了原来每次绘制都 CreateXxx + DeleteObject 的写法。
HPEN PerfPage::CachedPen(COLORREF c) const {
    auto it = penCache_.find(c);
    if (it != penCache_.end()) return it->second;
    HPEN p = CreatePen(PS_SOLID, 1, c);
    // 【不要缓存 NULL】CreatePen 失败时若把 NULL 存进表，该颜色此后
    // **永久**返回 NULL（即使 GDI 恢复也不重试），对应的线就永远画不出来。
    // 与 tmwindow_list.cpp 的基准实现对齐。
    if (p) penCache_.emplace(c, p);
    return p;
}

HBRUSH PerfPage::CachedBrush(COLORREF c) const {
    auto it = brushCache_.find(c);
    if (it != brushCache_.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    // 【不要缓存 NULL】同上。缓存 NULL 画刷的后果更难发现：
    // FillRect(NULL 画刷) 静默不填充，某块背景永远画不出来且无任何报错。
    if (b) brushCache_.emplace(c, b);
    return b;
}

void PerfPage::Fill(HDC hdc, const RECT& rc, COLORREF c) const {
    // 【性能】原来是 CreateSolidBrush + FillRect + DeleteObject，每调用一次
    // 建一个对象。本页每帧调用几十次（每张卡片背景、每块图表底、每个格子）。
    // 拿到 NULL 画刷时直接返回：FillRect 传 NULL 不会报错，只是不填充。
    HBRUSH b = CachedBrush(c);
    if (!b) return;
    FillRect(hdc, &rc, b);
}

void PerfPage::HLine(HDC hdc, int x1, int x2, int y, COLORREF c) const {
    // 【性能】网格线循环里每帧几十次调用，原实现每次都 CreatePen/DeleteObject。
    HPEN p = CachedPen(c);
    HGDIOBJ o = SelectObject(hdc, p);
    MoveToEx(hdc, x1, y, nullptr);
    LineTo(hdc, x2, y);
    SelectObject(hdc, o);
}

void PerfPage::VLine(HDC hdc, int x, int y1, int y2, COLORREF c) const {
    HPEN p = CachedPen(c);
    HGDIOBJ o = SelectObject(hdc, p);
    MoveToEx(hdc, x, y1, nullptr);
    LineTo(hdc, x, y2);
    SelectObject(hdc, o);
}

void PerfPage::DrawText2(HDC hdc, const wchar_t* s, RECT* rc, UINT fmt, COLORREF c) const {
    if (!s || !*s) return;
    SetTextColor(hdc, c);
    DrawTextW(hdc, s, -1, rc, fmt | DT_NOPREFIX);
}

// ============ 左侧列表 ============

void PerfPage::PaintSidebar(HDC hdc) const {
    // 【必须用 const_cast 的 const 方法】Paint 是 const，但侧栏项是按快照
    // 生成的可变缓存（items_ 声明为 mutable）。重建逻辑统一在非 const 的
    // EnsureSidebarItems 里，这里只负责"保证已经重建过"。
    const_cast<PerfPage*>(this)->EnsureSidebarItems();

    RECT side{ bounds_.left, bounds_.top,
               bounds_.left + kSidebarWidth, bounds_.bottom };
    Fill(hdc, side, kSidebarBg);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    // 侧栏几何。依据（docs/evidence/exec/_S_geo2_30001.tsv + S_duispec.md §1.2 / §3.2）：
    //   【UIFILE 声明】TmScrollViewer(dashSidebarScrollViewer) padding = rect(3rp,7rp,0rp,0rp)
    //     → top = 7rp、left = 3rp（旧值 top 13 / left 8 来自已证伪的 truth1.png）
    //   【UIFILE 声明】dashSidebarEntry padding = rect(0rp,0rp,3rp,0rp)（右留白 3rp）
    //   【UIFILE 声明】chartbox→dashSidebarActionButton padding = rect(10rp,5rp,9rp,10rp)
    //   【UIFILE 声明】chart 样式 width=60rp height=45rp、margin=rect(20rp,10rp,10rp,10rp)
    //   【UIFILE 未定义】卡片高度：条目高度由「迷你图 45rp + 行文字」撑出，UIFILE 不声明，
    //     所以 kSidebarCardH2/H3（61/68）仍是旧值，只作为布局步进使用。
    //   ⚠ 迷你图左沿存在**合成歧义**：chart 的 margin.left=20rp 与 ActionButton 的
    //     padding.left=10rp 是两层盒模型，UIFILE 没说迷你图是否是按钮的第一个子元素，
    //     所以 20 还是 30 无法定论（S_duispec.md §5.11 记了同一处歧义）。这里取 chart
    //     自身声明的 margin.left=20rp，不再叠加按钮 padding，避免双重内缩。
    const int padL = Rpx(kSidebarLeftPad);
    const int miniLeftOff = Rpx(20);          // chart 样式 margin.left = 20rp
    const int miniTopOff  = Rpx(5);           // ActionButton padding.top 5rp
    const int textXOff    = miniLeftOff + Rpx(kMiniChartW) + Rpx(10);  // 图表右缘 + margin.right 10rp

    int y = bounds_.top + Rpx(kSidebarTopPad) - sidebarScroll_;
    // 【分盘前置】旧代码 `if (item.bottom > bounds_.bottom) break;` 让超出的卡
    // 静默消失 —— 分盘成 8 张后网卡和 GPU 卡会被直接丢掉，且无法滚到。
    // 现在：整块侧栏设裁剪区，越界的卡**跳过绘制**（不是中断循环），
    // 滚动用 sidebarScroll_ 承担。
    const int savedDC = SaveDC(hdc);
    IntersectClipRect(hdc, side.left, bounds_.top + Rpx(kSidebarTopPad),
                      side.right, bounds_.bottom);

    std::vector<int> sbTops, sbHeights;
    SidebarLayout(sbTops, sbHeights);

    for (size_t i = 0; i < items_.size(); ++i) {
        const int cardH = sbHeights[i];
        const int cardTop = y + sbTops[i];

        RECT item{ side.left + padL, cardTop,
                   side.left + padL + kSidebarCardW, cardTop + cardH };
        // 完全在视口外就跳过（不是 break —— 下面的卡可能滚回来）
        if (item.bottom <= bounds_.top + Rpx(kSidebarTopPad)) continue;
        if (item.top >= bounds_.bottom) continue;

        // 高亮：selectedItem_ 是 items_ 的下标（由 OnClick / SyncSelectionToView 维护）。
        // 【P0-7】SyncSelectionToView 保证「正文页 ↔ 高亮卡片」始终一致，
        //   所以这里不需要再判 view_，否则两套判断会打架。
        bool sel = (static_cast<int>(i) == selectedItem_);
        bool hov = (static_cast<int>(i) == hoverItem_);

        if (sel)      Fill(hdc, item, kSidebarSel);
        else if (hov) Fill(hdc, item, kSidebarHover);

        // 迷你图表：声明尺寸 60rp × 45rp（含 padding-top 5rp，可视内容高 40rp）
        // 【已接线】「侧栏右键 → 隐藏图形」勾上时不画迷你图，只留文字。
        //   以前 hideSidebarGraph 只翻转 PerfToggles 里的一个 bool、绘制侧从不读，
        //   与「显示内核时间」同属一批假功能。
        if (!MenuBuilder::PerfToggles().hideSidebarGraph) {
            const int chartH = (std::min)(Rpx(kMiniChartH), cardH - miniTopOff);
            RECT mc{ item.left + miniLeftOff, item.top + miniTopOff,
                     item.left + miniLeftOff + Rpx(kMiniChartW), item.top + miniTopOff + chartH };
            PaintSidebarMiniChart(hdc, mc, items_[i]);
        }

        // 文字区
        const int textX = item.left + textXOff;
        const int textW = (side.left + kSidebarWidth) - textX - Rpx(3);
        if (textW > 10) {
            // L1 名称（16px，深色）
            // 【主题实测 GetThemeColor】sidebarLabel → gtf(TASKMANAGER,16,0) = YaHei UI 16px/400，
            //   fg = windowtext（UIFILE 该元素未引 gtc，取系统窗口文字色）。
            SelectObject(hdc, fontSide1_);
            RECT l1{ textX, item.top + 6, textX + textW, item.top + 26 };
            DrawText2(hdc, items_[i].name.c_str(), &l1,
                      DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS, RGB(0x00, 0x00, 0x00));

            // L2 主要数值（12px，黑）
            // 【主题实测 GetThemeColor】sidebarData → gtf(TASKMANAGER,17,0) = 12px/400
            SelectObject(hdc, fontSide2_);
            RECT l2{ textX, item.top + 25, textX + textW, item.top + 41 };
            DrawText2(hdc, items_[i].sub.c_str(), &l2,
                      DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS, RGB(0x00, 0x00, 0x00));

            // L3 明细（11px，灰）
            // 【无依据，沿用旧值】UIFILE 里侧栏没有第三档 gtf（sidebarDataHeader 也用 part 17 = 12px），
            //   11px 无出处；颜色 #707070 恰与 gtc(TASKMANAGER,17,0,3803) 的标签灰一致。
            if (!items_[i].detail.empty()) {
                SelectObject(hdc, fontSide3_);
                RECT l3{ textX, item.top + 41, textX + textW, item.top + 57 };
                DrawText2(hdc, items_[i].detail.c_str(), &l3,
                          DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS, kAxisText);
            }
        }

        // 选中态**没有**额外描边：kSidebarSel 已是 dtb(EXPLORER::LISTVIEW,1,6) 的纯填充值。

        y += cardH;   // 无间隙
    }

    RestoreDC(hdc, savedDC);

    // 侧栏右侧分隔线。
    // 【UIFILE 声明】dashSidebarDivider 的几何：padding = rect(0rp,7rp,0rp,0rp)，
    //   两段内容各 45rp × 8rp（sgraphic(31222/31223/31224) 贴图）。
    // 【主题未定义，沿用旧值】贴图不是纯色，取不到 RGB；现用 #E6E6E6 单线，起止仍按旧值
    //   （顶部 +23 / 底部 −18），UIFILE 只给了 padding.top=7rp 这一个数。
    VLine(hdc, side.right, bounds_.top + Rpx(7),
          (std::min)(bounds_.bottom - 18, bounds_.bottom),
          RGB(0xE6, 0xE6, 0xE6));
    SelectObject(hdc, old);
}

// ============ 标题块 ============

void PerfPage::PaintTitleBlock(HDC hdc, const RECT& rc, const wchar_t* title,
                               const wchar_t* sub, const wchar_t* rightInfo) const {
    // 【缺陷修复，保留】之前标题（28px）和副标题都在顶部 26px 内画，直接重叠。
    // 现在的排布是同一行左右并列：左边大字标题，右边稍小的说明文字。
    // 【UIFILE 声明】对应结构：chartInfo 下的 resourceTitle（part 14 = 29px, contentalign=bottomleft）
    //   与 resourceName（part 15 = 16px, contentalign=bottomright|endellipsis）是**兄弟元素**，
    //   靠 contentalign 分居左右（S_duispec.md §3.3）。
    // 【无依据，沿用旧值】34/6/14/10/30 这些像素偏移与右侧 220 留白无 UIFILE 值。
    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontTitle_));
    SetBkMode(hdc, TRANSPARENT);

    // 先量标题宽度，副标题从它右边开始
    SIZE st{};
    GetTextExtentPoint32W(hdc, title, static_cast<int>(wcslen(title)), &st);

    RECT tr{ rc.left, rc.top, rc.left + st.cx + 8, rc.top + 34 };
    DrawText2(hdc, title, &tr, DT_SINGLELINE | DT_LEFT, kTitleText);

    if (sub && *sub) {
        SelectObject(hdc, fontSub_);
        RECT sr{ rc.left + st.cx + 14, rc.top + 6, rc.right - 200, rc.top + 34 };
        DrawText2(hdc, sub, &sr, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS, kTitleText);
    }

    if (rightInfo && *rightInfo) {
        SelectObject(hdc, fontSmall_);
        RECT rr{ rc.right - 220, rc.top + 10, rc.right, rc.top + 30 };
        DrawText2(hdc, rightInfo, &rr, DT_SINGLELINE | DT_RIGHT, kSubText);
    }
    SelectObject(hdc, old);
}

// ============ 主图表 ============

void PerfPage::PaintChart(HDC hdc, const RECT& rcIn, const double* data, int count, int head,
                          const wchar_t* maxLabel, const wchar_t* minLabel,
                          COLORREF color, const double* overlay) const {
    // 【防御】窗口被压得很矮时上游算出的 bottom 可能小于 top（例如 CPU 页
    // statsTop = rc.bottom-200 在高度 < 350 时会反号）。这里统一归一化，
    // 避免网格循环用负数高度、以及上下两个标签画到对方位置上。
    RECT rc = rcIn;
    if (rc.bottom < rc.top) { int t = rc.top; rc.top = rc.bottom; rc.bottom = t; }
    if (rc.right  < rc.left) { int t = rc.left; rc.left = rc.right; rc.right = t; }
    if (rc.bottom - rc.top < 8 || rc.right - rc.left < 8) return;

    // 图表底色 = 纯白。
    // 【真原版截图实测】docs/reference/perf_mem_orig.png 图表内部（x=500/700/1000 多列扫描）
    //   除网格线 #ECDEF0 与曲线 #9528B4 外，全是 **#FFFFFF**；我方此前用 #F1F6FA（淡蓝灰），
    //   整块图表偏色。CPU 逻辑处理器格子的底色原本就是 #FFFFFF（kCoreBg），两者一致。
    Fill(hdc, rc, RGB(0xFF, 0xFF, 0xFF));

    // 网格色 = 该页主色的配套浅色。
    // 【真原版截图实测】内存页的主色是 #8B12AE（紫），网格线实测 #ECDEF0（浅紫）；
    // 横向间距 31px（实测线在 y=183/214/245/276/307/338/369/400/431，8 个间距全 31）。
    // 我方此前所有页都用 CPU 的 #E8F0F9（浅蓝）+ 16px 间距，于是内存页的网格是蓝的、还密一倍。
    // CHARTVIEW 主题给每页配了「主 + 浅」成对色（U_themecolors.md §3.1），这里按页面取色。
    const COLORREF gridColor = GridColorFor(view_);

    // 横向网格线：固定 31px 间距。
    // 此前是"10 条等分"：图表多高都画 10 条，图一矮就变成 16px 间距（密一倍），
    // 与原版的固定密度不符。顶边由边框承担，第一条网格线在 top+31。
    constexpr int kGridStepY = 31;
    for (int y = rc.top + kGridStepY; y < rc.bottom; y += kGridStepY) {
        HLine(hdc, rc.left, rc.right, y, gridColor);
    }

    // 纵向网格线：固定 50px 间距，首线在 left+17。
    // 【真原版截图实测】内存页竖线在 x=258/308/358/.../1208 —— 间距 **50px**，
    //   首线距左框（x=241）**17px**，共 19 根。横向 31、纵向 50，两者不同，不能一刀切。
    constexpr int kGridStepX = 50;
    constexpr int kGridPhaseX = 17;
    for (int x = rc.left + kGridPhaseX; x < rc.right; x += kGridStepX) {
        VLine(hdc, x, rc.top, rc.bottom, gridColor);
    }

    // 外框：页面主色 1px。
    // 【真原版截图实测】内存页边框是 #8B12AE（y=152 与 y=462 两条横线、x=241/x=1225 两条竖线），
    //   我方主图**完全没有边框**（全图搜 #8B12AE 只有侧栏迷你图的 683 个像素，主图矩形内 0）。
    {
        HGDIOBJ op0 = SelectObject(hdc, GetStockObject(DC_PEN));
        HGDIOBJ ob0 = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        SetDCPenColor(hdc, color);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, ob0);
        SelectObject(hdc, op0);
    }

    // 纵轴文字：**在图表矩形之外**。
    //   上限标签 → 图表上方一行、右对齐到图表右缘
    //   下限标签 → 图表下方一行、左对齐到图表左缘
    // 【UIFILE 声明】每张图的 title 元素放上限、`*_bottom` 元素放下限，两者都不在
    // graph 元素内部（S_duispec.md §3.4 / §3.5 / §3.6 的元素树）：
    //   primaryCpuChart_bottom → RateLabelCpu   (class=lheader)
    //   memoryUsageGraph_bottom → RateLabelMem  (class=lheader)
    //   diskTime_bottom → RateLabelDiskTime / diskRate_bottom → RateLabelDiskRate
    //   primaryNetChart_bottom → RateLabelNet
    // 【主题实测 GetThemeColor】这两处文字用 lheader/rheader → gtc(TASKMANAGER,44,0,3803)
    //   = #707070，与 kAxisText 同值。
    // 【无依据，沿用旧值】「上一行 16px / 下一行 15px」的行高偏移没有 UIFILE 值，
    //   旧值来自已证伪的 truth2；这里保留以免改动行距。
    {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        if (maxLabel && *maxLabel) {
            RECT t{ rc.right - 80, rc.top - 16, rc.right, rc.top };
            DrawText2(hdc, maxLabel, &t, DT_SINGLELINE | DT_RIGHT, kAxisText);
        }
        if (minLabel && *minLabel) {
            RECT b{ rc.left, rc.bottom, rc.left + 80, rc.bottom + 15 };
            DrawText2(hdc, minLabel, &b, DT_SINGLELINE | DT_LEFT, kAxisText);
        }
        SelectObject(hdc, old);
    }

    if (count < 2) return;

    // 曲线（60 点 → 像素）
    const int w = rc.right - rc.left - 4;
    const int h = rc.bottom - rc.top - 4;
    if (w < 8 || h < 8) return;

    // 把环形缓冲的一段映射成折线。
    // 抽成 lambda 是因为「显示内核时间」需要按**同一套时间轴与刻度**再画一条，
    // 两条曲线的 x/y 映射必须逐像素一致，否则叠加出来会错位。
    auto buildPts = [&](const double* src) {
        std::vector<POINT> p;
        p.reserve(count);
        for (int i = 0; i < count; ++i) {
            int idx = (head - count + i + PerfHistory::kPoints * 2) % PerfHistory::kPoints;
            double v = src[idx];
            if (v > 100) v = 100;
            if (v < 0) v = 0;
            int px = rc.left + 2 + (count > 1 ? i * (w - 1) / (count - 1) : 0);
            int py = rc.bottom - 2 - static_cast<int>(v / 100.0 * (h - 1));
            p.push_back(POINT{ px, py });
        }
        return p;
    };

    std::vector<POINT> pts = buildPts(data);

    // 不做曲线下填充：底色现在是纯白，填充色 kChartFill=#F1F6FA 反而会画出一块
    // 灰蓝色带；原版内存页曲线右侧（x=1100..1224）实测曲线之上是 #F4F2F4，
    // 是「已缓存」浅色叠加区而不是曲线填充。为避免凭空造出一块原版没有的色带，仍然不画。

    // 曲线本身：1px。
    // 【无依据，沿用旧值】线宽 1px：CHARTV.dll 的画笔宽度 UIFILE/主题都不给，旧值来自
    //   已证伪的 truth1，但 1px 与逻辑处理器格子（同样 1px）一致，保留。
    // 【性能】改用缓存画笔（颜色是每页固定主色，不是动态插值）。
    HPEN pen = CachedPen(color);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    Polyline(hdc, pts.data(), static_cast<int>(pts.size()));

    // 内核时间叠加曲线（「查看 → 显示内核时间」）。
    // 【颜色无原版像素证据】原版这条线的色值没有从任何真原版截图里测到
    //   （手头三张原版截图都没勾这一项）。这里用与主曲线同色系的深色，
    //   保证在白色底 + 浅色网格上可辨，并且不冒充「已实测」。
    //   若要精确对齐，需要一张勾选了「显示内核时间」的原版截图。
    if (overlay) {
        std::vector<POINT> pts2 = buildPts(overlay);
        const COLORREF kKernelLine = RGB(0x0A, 0x4A, 0x70);
        HPEN pen2 = CachedPen(kKernelLine);
        HGDIOBJ op2 = SelectObject(hdc, pen2);
        Polyline(hdc, pts2.data(), static_cast<int>(pts2.size()));
        SelectObject(hdc, op2);
    }

    SelectObject(hdc, oldp);
}

// ============ 逻辑处理器格子 ============
//
// Win10 的「逻辑处理器」视图：每格是一张**微型面积图**，不是色块 + 大号百分比。
// 我之前做成了后者（那是 Win8 的样式），被用户指出。正确做法：
//
//   ┌──────────────────────┐
//   │ CPU 0                │  ← 左上角 9px 灰色小标签
//   │    ╱╲    ╱╲          │
//   │   ╱  ╲__╱  ╲___      │  ← 60 点历史面积图，蓝色描边 + 浅蓝填充
//   │ ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁  │  ← 底部基线
//   └──────────────────────┘
//
// 每格没有百分比数字 —— 数值靠图形高度表达。这才是 Win10 的样子。

namespace {

// 单格面积图的配色。
//
// 【UIFILE 声明】格子的**几何**可查（S_duispec.md §1.4 / §3.4）：
//   cpuBlock        height = 17rp @0x5978
//   cpuBlock        borderthickness = rect(0rp,0rp,0rp,1rp) @0x597c（背景块 B）/ rect(0rp,0rp,1rp,0rp) @0x598c（R）
//   cpuBlockData    borderthickness = rect(1rp,1rp,1rp,1rp) @0x59a4、padding = rect(5rp,0rp,5rp,0rp) @0x59a8
//   cpuHeatGrid     bordercolor = gtc(TASKMANAGER,38,0,3802) = #117DBB、borderthickness = rect(1,1,1,1)
//   logicalCpuHost / heatMapCpuHost / numaCpuHost  height = 80rp
// 格子颜色现在有**真原版截图的逐像素实测值**（见下方，取代了此前「主题 0 命中、无来源」
// 的旧注释 —— 旧的 #1B82C2/#6AAED4/#E8F0F9/#DEECF5 全部反解自已证伪的 truth1）。
//
// 绘制顺序：填充 → 横网格 → 竖网格 → 曲线 → 边框
//
// 【颜色值全部改为真原版截图像素实测】
// docs/reference/perf_cpu_orig.png 的 CPU0 格子（x=242..483, y=153..247）逐像素统计：
//   边框      top/bottom/left/right 四条边实测 #117DBB（240/240 像素）
//   横网格    163/172/182/191/200/210/219/229/238 九条，颜色 #D9EAF4（间距 ~9px，10 带）
//   竖网格    283/324/365/406/446 五条，颜色 #D9EAF4（间距 ~41px，6 带）
//   曲线      #117DBB（渐变边缘 #2E8CC4 / #66ABD3 是抗锯齿）
//   曲线下填充 #F1F6FA
//   格子底     #FFFFFF
// 旧值（#1B82C2 / #6AAED4 / #E8F0F9 / #DEECF5）反解自已证伪的 truth1.png。
constexpr COLORREF kCoreLine   = RGB(0x11, 0x7D, 0xBB);   // 原版实测
constexpr COLORREF kCoreFill   = RGB(0xF1, 0xF6, 0xFA);   // 原版实测
constexpr COLORREF kCoreBg     = RGB(0xFF, 0xFF, 0xFF);   // 原版实测（UIFILE 也是 window 色）
constexpr COLORREF kCoreEdge   = RGB(0x11, 0x7D, 0xBB);   // 原版实测：与曲线同色
constexpr COLORREF kGridH      = RGB(0xD9, 0xEA, 0xF4);   // 原版实测：CPU 配套浅蓝
constexpr COLORREF kGridV      = RGB(0xD9, 0xEA, 0xF4);   // 原版实测：横竖同色

} // namespace

// 一格「历史面积图」小卡：可选左上角标签 + 60 点曲线 + 网格 + 边框。
// vals 是**按时间正序**（0 = 最旧、n-1 = 最新）的 0..100 值。
// 【为什么单独抽出来】逻辑处理器格子和 NUMA 节点格子除了「每条曲线的数据来自哪」
//   完全一样（同样 1rp 网格、同样 1px 曲线、同样填充与边框），
//   复制一遍会导致两处配色/网格数量以后各自漂移。
void PerfPage::PaintHistCell(HDC hdc, const RECT& cell, const double* vals, int n,
                             const wchar_t* label) const {
    Fill(hdc, cell, kCoreBg);

    RECT plot{ cell.left + 1, cell.top + 1, cell.right - 1, cell.bottom - 1 };
    if (label && *label) {
        // 标签占顶部 12px，绘图区下移
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontTiny_));
        SetBkMode(hdc, TRANSPARENT);
        RECT lr{ cell.left + 3, cell.top + 1, cell.right - 2, cell.top + 13 };
        DrawText2(hdc, label, &lr, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS, kAxisText);
        SelectObject(hdc, old);
        plot.top = cell.top + 13;
    }

    const int pw = plot.right - plot.left;
    const int ph = plot.bottom - plot.top;
    const bool haveCurve = (vals && n >= 2 && pw > 4 && ph > 4);

    // 格子被压得很矮时（带标签的行会把绘图区再压低 13px），ph 可能 <= 0。
    // 那种情况下只画边框，不画网格/曲线 —— 否则网格线的 yy 会落到格子外面去。
    if (pw > 0 && ph > 0) {
        std::vector<POINT> pts;
        if (haveCurve) {
            pts.reserve(n);
            for (int k = 0; k < n; ++k) {
                double v = vals[k];
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                int px = plot.left + (n > 1 ? k * (pw - 1) / (n - 1) : 0);
                int py = plot.bottom - static_cast<int>(v / 100.0 * (ph - 1) + 0.5);
                if (py < plot.top) py = plot.top;
                pts.push_back(POINT{ px, py });
            }

            // 填充（曲线下方）
            std::vector<POINT> poly = pts;
            poly.push_back(POINT{ pts.back().x, plot.bottom });
            poly.push_back(POINT{ pts.front().x, plot.bottom });
            // 【性能】20 核 × 每次重绘 = 原来每帧建 20 个画刷。kCoreFill 是常量。
            HBRUSH fb = CachedBrush(kCoreFill);
            HGDIOBJ ob = SelectObject(hdc, fb);
            HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
            Polygon(hdc, poly.data(), static_cast<int>(poly.size()));
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
        }

        // 网格线（画在填充之上、曲线之下）
        {
            HGDIOBJ op = SelectObject(hdc, GetStockObject(DC_PEN));
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            // 横线 9 条：10%..90%，0% 和 100% 由边框充当
            SetDCPenColor(hdc, kGridH);
            for (int d = 1; d <= 9; ++d) {
                int yy = plot.bottom - (ph * d) / 10;
                MoveToEx(hdc, plot.left, yy, nullptr);
                LineTo(hdc, plot.right, yy);
            }

            // 竖线 5 条：把格子横向分成 **6 带**。
            // 【真原版截图实测】格宽 241px 时竖线在 283/324/365/406/446（相对格左 41/82/123/164/205），
            //   间距约 41px ≈ 241/6 —— 所以是 6 等分、5 条线。旧实现是 4 等分 3 条线，偏稀。
            SetDCPenColor(hdc, kGridV);
            for (int d = 1; d <= 5; ++d) {
                int xx = plot.left + (pw * d) / 6;
                MoveToEx(hdc, xx, plot.top, nullptr);
                LineTo(hdc, xx, plot.bottom);
            }

            SelectObject(hdc, ob);
            SelectObject(hdc, op);
        }

        // 曲线（1px）
        if (haveCurve) {
            HPEN pen = CachedPen(kCoreLine);   // 【性能】同上，20 核 × 每帧
            HGDIOBJ oldp = SelectObject(hdc, pen);
            Polyline(hdc, pts.data(), static_cast<int>(pts.size()));
            SelectObject(hdc, oldp);
        }
    }

    // 边框
    HGDIOBJ op2 = SelectObject(hdc, GetStockObject(DC_PEN));
    SetDCPenColor(hdc, kCoreEdge);
    HGDIOBJ ob2 = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, cell.left, cell.top, cell.right, cell.bottom);
    SelectObject(hdc, ob2);
    SelectObject(hdc, op2);
}

void PerfPage::PaintLogicalCpuGrid(HDC hdc, const RECT& rc) const {
    if (!snap_ || snap_->cores.empty()) return;

    const int n = static_cast<int>(snap_->cores.size());
    const int availW = static_cast<int>(rc.right - rc.left);
    const int availH = static_cast<int>(rc.bottom - rc.top);
    if (availW < 40 || availH < 40) return;

    // 网格行列：**列数 = ceil(sqrt(n))**，行数 = ceil(n / 列数)。
    // 【真原版截图实测】docs/reference/perf_cpu_orig.png（8 核 16 线程机器）：
    //   16 格排成 4 列 × 4 行 —— ceil(sqrt(16)) = 4，与实测完全一致。
    //   列边界 x=242/483 | 489/729 | 735/974 | 981/1221（格宽 241、缝 5），
    //   行边界 y=153/247 | 252/346 | 351/445 | 450/544（格高 94、缝 5），
    //   即 gapX=5 / gapY=5。
    // 【为什么不再按 144px 自适应】旧算法用「每格至少 144px 宽」去逼近，
    //   在 980px 可用宽度下会算出 **6 列**（980/6=163≥144），16 核排成 6×3，
    //   与原版的 4×4 完全不同。ceil(sqrt(n)) 既复现 16 核的 4×4，
    //   又天然给出 20 核的 5 列 × 4 行（见 docs/specs/MM_perf.md）。
    int cols = 1;
    while (cols * cols < n) ++cols;            // cols = ceil(sqrt(n))
    if (cols > n) cols = n;
    // 宽度兜底：窗口被压得很窄时按可用宽度砍列数，避免格子挤成一条线。
    while (cols > 1 && availW / cols < 60) --cols;
    int rows = (n + cols - 1) / cols;

    const int gapX = 5, gapY = 5;
    // 【真原版截图实测】16 核原版：格宽 241（x 242..483）、格高 94（y 153..247），
    //   横向间隙 488-483=5、纵向间隙 252-247=5。旧值 gapX=5/gapY=4 里横向本来是对的，
    //   纵向差 1，这里统一成 5。
    int cellW = (availW - gapX * (cols - 1)) / (cols > 0 ? cols : 1);
    int cellH = (availH - gapY * (rows - 1)) / (rows > 0 ? rows : 1);
    if (cellW < 40) cellW = 40;
    if (cellH < 30) cellH = 30;

    std::vector<double> vals;
    const int drawn = (hist_.count < PerfHistory::kPoints) ? hist_.count
                                                           : PerfHistory::kPoints;
    vals.resize(drawn > 0 ? drawn : 0);

    for (int i = 0; i < n; ++i) {
        int r = i / cols;
        int c = i % cols;
        int x = rc.left + c * (cellW + gapX);
        int y = rc.top + r * (cellH + gapY);
        RECT cell{ x, y, x + cellW, y + cellH };
        if (cell.bottom > rc.bottom) break;

        for (int k = 0; k < drawn; ++k) vals[k] = hist_.AtCore(i, k);
        // 逻辑处理器视图**不画百分比数字**（Win10 的样子），但 UIFILE 里
        // cpuBlockData 有数值元素 —— 这里保留原实现的「无标签」观感，传 nullptr。
        PaintHistCell(hdc, cell, vals.data(), drawn, nullptr);
    }
}

// NUMA 节点视图。
//
// 【为什么能做，不需要采集层】NUMA 节点只是「逻辑处理器 → 节点」的静态映射，
//   GetNumaProcessorNodeEx 一次调用就能拿到，不涉及任何新的性能计数器。
//   每格画的是该节点所有逻辑处理器在 hist_.coreHist 里的**逐点平均**，
//   也就是「这个 NUMA 节点的利用率」—— 与逻辑处理器格子同源、同量纲。
// 【单节点机器】原版的 NUMA 视图在单节点机器上仍然可用（显示一格）。
//   这里保持一致：只有一个节点也画，不特殊处理，免得用户以为菜单无效。
void PerfPage::PaintNumaCpuGrid(HDC hdc, const RECT& rc) const {
    if (!snap_ || snap_->cores.empty()) return;

    const int nCores = static_cast<int>(snap_->cores.size());
    if (nCores > PerfHistory::kMaxCores) return;   // coreHist 容量保护

    const int availW = static_cast<int>(rc.right - rc.left);
    const int availH = static_cast<int>(rc.bottom - rc.top);
    if (availW < 40 || availH < 40) return;

    // 把逻辑处理器分到节点。GetNumaProcessorNodeEx 的 Processor 索引与
    // NtQuerySystemInformation(8) 返回的 core 顺序一致（都是 0..N-1 的逻辑处理器号）。
    // 【证据缺口】这台机器只有 1 个 NUMA 节点（单路 E5），无法实测多节点布局；
    //   所以下面是「按节点号分组取平均」的通用算法，不依赖节点数。
    struct NumaGroup { std::vector<int> cores; };
    std::vector<NumaGroup> groups;
    for (int i = 0; i < nCores; ++i) {
        // PROCESSOR_NUMBER 的 Number 是**组内**编号（0..63），跨组机器必须拆组，
        // 否则第 64 个核会被当成 Number=64 溢出成 BYTE。
        PROCESSOR_NUMBER pn{};
        pn.Group  = static_cast<WORD>(i / 64);
        pn.Number = static_cast<BYTE>(i % 64);
        USHORT node = 0;
        if (!GetNumaProcessorNodeEx(&pn, &node)) {
            node = 0;   // 查询失败就归到节点 0，不能让格子凭空消失
        }
        if (static_cast<size_t>(node) >= groups.size()) {
            groups.resize(static_cast<size_t>(node) + 1);
        }
        groups[node].cores.push_back(i);
    }
    // 去掉中间的空节点（节点号可能不连续）
    std::vector<const NumaGroup*> live;
    for (const auto& g : groups) {
        if (!g.cores.empty()) live.push_back(&g);
    }
    if (live.empty()) return;

    // 布局：与逻辑处理器网格同一套尺寸规则，但格子数量是节点数（通常 1~2）。
    int cols = 4;
    while (cols < 8 && availW / (cols + 1) >= 144) ++cols;
    while (cols > 1 && availW / cols < 144) --cols;
    const int nNodes = static_cast<int>(live.size());
    if (cols > nNodes) cols = nNodes;
    int rows = (nNodes + cols - 1) / cols;

    const int gapX = 5, gapY = 4;
    int cellW = (availW - gapX * (cols - 1)) / (cols > 0 ? cols : 1);
    int cellH = (availH - gapY * (rows - 1)) / (rows > 0 ? rows : 1);
    if (cellW < 40) cellW = 40;
    if (cellH < 30) cellH = 30;

    const int drawn = (hist_.count < PerfHistory::kPoints) ? hist_.count
                                                           : PerfHistory::kPoints;
    std::vector<double> vals(static_cast<size_t>(drawn > 0 ? drawn : 0));

    for (int ni = 0; ni < nNodes; ++ni) {
        int r = ni / cols;
        int c = ni % cols;
        int x = rc.left + c * (cellW + gapX);
        int y = rc.top + r * (cellH + gapY);
        RECT cell{ x, y, x + cellW, y + cellH };
        if (cell.bottom > rc.bottom) break;

        const auto& g = *live[ni];
        // 逐点对节点内所有核取平均
        for (int k = 0; k < drawn; ++k) {
            double sum = 0.0;
            for (int core : g.cores) sum += hist_.AtCore(core, k);
            vals[k] = g.cores.empty() ? 0.0
                                      : sum / static_cast<double>(g.cores.size());
        }
        wchar_t label[64];
        swprintf_s(label, L"NUMA 节点 %d（%d 个处理器）", ni,
                   static_cast<int>(g.cores.size()));
        PaintHistCell(hdc, cell, vals.data(), drawn, label);
    }
}

// ============ 统计行 ============

void PerfPage::PaintStatsRows(HDC hdc, const RECT& rc) const {
    if (!snap_) return;

    // 布局依据（docs/specs/S_duispec.md §3.4，UIFILE 元素树）：
    //   static_cpu  (rowlayout(1))  —— 右侧静态属性，12 组 label+value 平铺
    //   dynamic_cpu —— 动态值：cpuUsage(margin rect(20,0,0,0)) / cpuSpeed(margin rect(0,8,0,0)) /
    //                  process(20,0,0,0) / thread(20,0,0,0) / handle(0,8,0,0) / cpuUptime
    //   【UIFILE 声明】组间横距 20rp、换行处纵距 8rp（_S_geo2_30001.tsv @0x5582 / @0x55bc）
    //   【主题实测 GetThemeColor】dynamicLabel → gtc(TASKMANAGER,17,0,3803)=#707070（标签灰）；
    //                  dynamicData → gtc(TASKMANAGER,19,0,3803)=#000000（数值黑）
    //   【主题实测 GetThemeFont】part 17 = 12px/400，part 19 = 21px/400，**都不是粗体**
    //   【UIFILE 未定义】「标签在上 / 数值在下」的两行堆叠方式、以及右块 8 行 KV 的行高 19px，
    //      UIFILE 只给 margin（20/8），没有行高；19/46 这些数是既有实现。

    if (view_ != PerfView::Cpu) {
        // 非 CPU 页仍用原来的单行统计（那些页的规格 UIFILE 未逐项声明）
        PaintStatsRowsGeneric(hdc, rc);
        return;
    }

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    // 【主题实测 GetThemeColor】dynamicLabel/staticLabel = #707070（旧值 #6D6D6D 无出处）
    const COLORREF kLabel = RGB(0x70, 0x70, 0x70);
    // 【主题实测 GetThemeColor】dynamicData / staticData = #000000
    const COLORREF kValue = RGB(0x00, 0x00, 0x00);

    // 画一个「标签 + 数值」盒子，返回盒子宽度
    auto Box = [&](int x, int y, const wchar_t* label, const wchar_t* value) -> int {
        SelectObject(hdc, fontSmall_);            // 12px
        SIZE sl{};
        GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &sl);
        RECT lr{ x, y, x + 400, y + 15 };
        DrawText2(hdc, label, &lr, DT_SINGLELINE | DT_LEFT, kLabel);

        SelectObject(hdc, fontStat_);             // 21px
        SIZE sv{};
        GetTextExtentPoint32W(hdc, value, static_cast<int>(wcslen(value)), &sv);
        RECT vr{ x, y + 17, x + 400, y + 40 };
        DrawText2(hdc, value, &vr, DT_SINGLELINE | DT_LEFT, kValue);

        return (sl.cx > sv.cx ? sl.cx : sv.cx);
    };

    wchar_t b1[64], b2[64], b3[64];

    // ---- 第一组：利用率 / 速度 ----
    int y = rc.top;
    {
        swprintf_s(b1, L"%.0f%%", snap_->cpuTotalPercent);
        int w1 = Box(rc.left, y, L"利用率", b1);

        swprintf_s(b2, L"%.2f GHz", snap_->cpuCurrentMhz / 1000.0);
        Box(rc.left + w1 + 20, y, L"速度", b2);
    }

    // ---- 第二组：进程 / 线程 / 句柄 ----
    y += 46;
    {
        swprintf_s(b1, L"%u", snap_->totalProcesses);
        int w1 = Box(rc.left, y, L"进程", b1);

        swprintf_s(b2, L"%u", snap_->totalThreads);
        int w2 = Box(rc.left + w1 + 20, y, L"线程", b2);

        swprintf_s(b3, L"%llu",
                   static_cast<unsigned long long>(snap_->totalHandles));
        Box(rc.left + w1 + 20 + w2 + 20, y, L"句柄", b3);
    }

    // ---- 第三组：正常运行时间 ----
    y += 46;
    {
        wchar_t up[32];
        FmtUptime(snap_->uptimeSeconds, up, 32);
        Box(rc.left, y, L"正常运行时间", up);
    }

    // ---- 右块：8 行键值对 ----
    {
        const int rx = rc.right - 330;
        int ry = rc.top;
        auto KV = [&](const wchar_t* label, const wchar_t* value) {
            RECT r{ rx, ry, rc.right, ry + 19 };
            SelectObject(hdc, fontSmall_);
            SetTextColor(hdc, kLabel);
            // 标签左、值右：用两次绘制实现
            DrawTextW(hdc, label, -1, &r, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);

            RECT vr{ rx, ry, rc.right, ry + 19 };
            SetTextColor(hdc, kValue);
            DrawTextW(hdc, value, -1, &vr, DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX);

            ry += 19;
        };

        swprintf_s(b1, L"%.2f GHz", snap_->cpuBaseMhz / 1000.0);
        KV(L"基准速度:", b1);

        swprintf_s(b1, L"%u", snap_->sockets);
        KV(L"插槽:", b1);

        swprintf_s(b1, L"%u", snap_->physicalCores);
        KV(L"内核:", b1);

        swprintf_s(b1, L"%u", snap_->logicalCores);
        KV(L"逻辑处理器:", b1);

        KV(L"虚拟化:", snap_->virtualizationEnabled ? L"已启用" : L"已禁用");

        if (snap_->l3CacheBytes) {
            swprintf_s(b1, L"%.0f KB", snap_->l1CacheBytes / 1024.0);
            KV(L"L1 缓存:", b1);
            swprintf_s(b1, L"%.1f MB", snap_->l2CacheBytes / 1048576.0);
            KV(L"L2 缓存:", b1);
            swprintf_s(b1, L"%.1f MB", snap_->l3CacheBytes / 1048576.0);
            KV(L"L3 缓存:", b1);
        }
    }

    SelectObject(hdc, old);
}

// 非 CPU 页的单行统计（这些页的精确规格本次未测，保持原有行为）
void PerfPage::PaintStatsRowsGeneric(HDC hdc, const RECT& rc) const {
    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
    SetBkMode(hdc, TRANSPARENT);

    int y = rc.top;
    const int lineH = 20;
    wchar_t buf[256];

    if (view_ == PerfView::Memory) {
        wchar_t total[64], inUse[64], avail[64], cached[64], committed[64];
        FmtBytesGB(snap_->mem.totalBytes, total, 64);
        FmtBytesGB(snap_->mem.inUseBytes, inUse, 64);
        FmtBytesGB(snap_->mem.availableBytes, avail, 64);
        FmtBytesGB(snap_->mem.cachedBytes, cached, 64);
        FmtBytesGB(snap_->mem.committedBytes, committed, 64);

        swprintf_s(buf, L"使用中  %ls", inUse);
        RECT r1{ rc.left, y, rc.left + 240, y + lineH };
        DrawText2(hdc, buf, &r1, DT_SINGLELINE | DT_LEFT, kTitleText);
        swprintf_s(buf, L"可用  %ls", avail);
        RECT r2{ rc.left + 220, y, rc.left + 440, y + lineH };
        DrawText2(hdc, buf, &r2, DT_SINGLELINE | DT_LEFT, kTitleText);
        swprintf_s(buf, L"已缓存  %ls", cached);
        RECT r3{ rc.left + 420, y, rc.left + 660, y + lineH };
        DrawText2(hdc, buf, &r3, DT_SINGLELINE | DT_LEFT, kTitleText);
        y += lineH + 2;
        swprintf_s(buf, L"已提交  %ls / %ls", committed, total);
        RECT r4{ rc.left, y, rc.left + 400, y + lineH };
        DrawText2(hdc, buf, &r4, DT_SINGLELINE | DT_LEFT, kSubText);
    }

    SelectObject(hdc, old);
}

// ============ 各页 ============

void PerfPage::PaintCpuPage(HDC hdc, const RECT& rc) const {
    // 标题区结构依据 UIFILE 元素树（docs/specs/S_duispec.md §3.3 / §3.4）：
    //   performancetab_tables → detailInfo → chartAndTitle → chartInfo
    //     ├ resourceTitle  gtf(TASKMANAGER,14,0) = YaHei UI -29px/400, fg=#000000, contentalign=bottomleft
    //     └ resourceName   gtf(TASKMANAGER,15,0) = 16px/400, fg=#000000,
    //                      margin=rect(15rp,0rp,0rp,0rp), padding=rect(0,0,0,3rp),
    //                      contentalign=bottomright|endellipsis
    // 【UIFILE 声明】resourceTitle 与 resourceName 是 chartInfo 的两个**兄弟**元素（同一容器内），
    //   不是「标题 + 绝对偏移的副标题」；两者都用 contentalign 定位。
    // 【无依据，沿用旧值】下面 +94 / +12..+28 / rc.right-190 这些偏移与矩形没有 UIFILE 值，
    //   全部反解自已证伪的 truth1.png。保留仅为不改变现有排版，
    //   真正的对齐语义应改成 contentalign=bottomleft（标题）+ bottomright（处理器名）。
    {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontTitle_));
        SetBkMode(hdc, TRANSPARENT);
        RECT tr{ rc.left, rc.top, rc.left + 90, rc.top + 34 };
        DrawText2(hdc, L"CPU", &tr, DT_SINGLELINE | DT_LEFT, kTitleText);

        // 处理器名（左对齐紧跟标题）
        // 【UIFILE 声明】resourceName margin.left = 15rp（相对 chartInfo 内容盒）
        // 【无依据，沿用旧值】`+94` 的绝对值偏移无 UIFILE 依据
        SelectObject(hdc, fontSub_);
        RECT nr{ rc.left + Rpx(15) + 79, rc.top + 6, rc.right - 210, rc.top + 34 };
        DrawText2(hdc, snap_ ? snap_->cpuName.c_str() : L"", &nr,
                  DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS, kTitleText);

        // 「逻辑处理器: N」—— 右对齐
        // 【无依据，沿用旧值】宽度/位置来自已证伪的 truth1；12px 字号有旁证
        //   （TEXTSTYLE,4,0 = 12px），颜色 kSubText 沿用旧值（主题 part 17 = #707070）。
        {
            wchar_t cnt[48];
            int n = snap_ ? static_cast<int>(snap_->logicalCores) : 0;
            swprintf_s(cnt, L"逻辑处理器: %d", n);
            SelectObject(hdc, fontBody_);          // 12px
            RECT cr{ rc.right - 190, rc.top + 12, rc.right, rc.top + 28 };
            DrawText2(hdc, cnt, &cr, DT_SINGLELINE | DT_RIGHT, kSubText);
        }

        SelectObject(hdc, old);
    }

    // 「总体利用率」模式按钮 —— **只在 Overall 视图下绘制**。
    // 【真原版截图实测】docs/reference/perf_cpu_orig.png（默认的逻辑处理器网格视图）
    //   右上角 x=900..1240 / y=90..135 区间内唯一的深色像素是「逻辑处理器: 16」这行
    //   文字（bbox x=932..1218, y=110..125），**没有任何按钮的底/边框像素**。
    //   即原版的逐核网格视图里根本没有这个切换控件。
    // 【改动】以前这个按钮在所有模式下都画，包括逻辑处理器网格 —— 与原版不符，
    //   而且它占着网格右上方的位置。现在只有 Overall 视图才画；
    //   其它模式要切回来请走右键菜单「将图形更改为」(MENU 30206)，与原版一致。
    // 【无依据，沿用旧值】按钮外观（#F3F3F3 底 / #D0D0D0 框 / 矩形位置）仍无 UIFILE 依据。
    //   注：主题里 Button 类的渲染值是 #E1E1E1 芯 + #ADADAD 边（U_themecolors.md §4.5），
    //   比现用的 #F3F3F3/#D0D0D0 更深，但那是「按钮」不是「下拉选择器」，语义不同，不动。
    if (chart_ == ChartMode::Overall) {
        RECT br{ rc.right - 198, rc.top + 30, rc.right - 7, rc.top + 47 };
        Fill(hdc, br, RGB(0xF3, 0xF3, 0xF3));
        HGDIOBJ op = SelectObject(hdc, GetStockObject(DC_PEN));
        SetDCPenColor(hdc, RGB(0xD0, 0xD0, 0xD0));
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, br.left, br.top, br.right, br.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        HFONT of = static_cast<HFONT>(SelectObject(hdc, fontBody_));
        SetBkMode(hdc, TRANSPARENT);
        DrawText2(hdc, CpuChartModeName(), &br,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER, kTitleText);
        SelectObject(hdc, of);
    }

    int statsTop = rc.bottom - 200;


    // 主图 / 逻辑处理器网格 / NUMA 节点网格。
    // 【UIFILE 声明】chartArea 的位置参数是 rect(15rp,0rp,0rp,0rp)（CPU/内存/GPU 三页共用，
    //   _S_geo2_30001.tsv @0x572a），主图容器 sheet="staticResourceChart" height=80rp（@0x320）。
    // 【UIFILE 声明】chartArea 的 layoutpos=client —— 主图会被拉伸填满剩余空间，
    //   所以 80rp 是**基准/最小高**而不是最终像素高（S_duispec.md §5.8）。
    //   Rpx(kChartHeight)=80 作为初始高；下方两个分支再按可用空间拉伸/压缩。
    // 【UIFILE 未定义】垂直偏移 `rc.top+56`、右侧内缩 `−8`、下让位 `−10 / −90` 为既有值。
    RECT chart{ rc.left + Rpx(kChartPad), rc.top + 56, rc.right - 8,
                rc.top + 56 + Rpx(kChartHeight) };
    // 【P0-6】三个可绘制的 CPU 子视图：
    //   Logical → 逐逻辑处理器网格；Numa → 逐 NUMA 节点网格；其余（含 GPU 专用的
    //   SingleEngine/MultiEngine）→ 单条总利用率曲线。
    const bool gridView = (chart_ == ChartMode::Logical || chart_ == ChartMode::Numa);
    if (gridView) {
        // 网格上方的两个标签。
        // 【UIFILE 声明】元素名与文案都在：cpuChartTitle class=lheader content=34280「60 秒内的利用率 %」，
        //   同一行右侧 rheader content=33836「100%」（S_duispec.md §3.4）。
        // 【主题实测 GetThemeColor】两者都是 lheader/rheader → gtc(TASKMANAGER,44,0,3803) = #707070。
        // 【无依据，沿用旧值】标签行高 16px 与 x 偏移无 UIFILE 值。
        {
            HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
            SetBkMode(hdc, TRANSPARENT);

            RECT lr{ chart.left + 1, chart.top - 16, chart.left + 200, chart.top };
            DrawText2(hdc, L"60 秒内的利用率 %", &lr,
                      DT_SINGLELINE | DT_LEFT, kAxisText);

            RECT rr{ chart.right - 60, chart.top - 16, chart.right, chart.top };
            DrawText2(hdc, L"100%", &rr,
                      DT_SINGLELINE | DT_RIGHT, kAxisText);

            SelectObject(hdc, old);
        }

        // 网格占据整块主图区域。
        // 【UIFILE 声明】logicalCpuHost / heatMapCpuHost / numaCpuHost height = 80rp，
        //   cpuGrid layoutpos=client（会拉伸）。下方 ±10/120 的限位是既有防御逻辑，未动。
        chart.bottom = statsTop - 10;
        if (chart.bottom < chart.top + 120) chart.bottom = chart.top + 120;
        if (chart_ == ChartMode::Numa) {
            PaintNumaCpuGrid(hdc, chart);
        } else {
            PaintLogicalCpuGrid(hdc, chart);
        }
    } else {
        // 总体视图（以及 GPU 专用的 SingleEngine/MultiEngine 落回 CPU 页时）：
        // chartArea 是 layoutpos=client，主图应**填满** chartArea 剩余空间，
        // 80rp 只是声明基准（最小高）。所以这里取到 statsTop-90，低于 80rp 才回退到 80rp。
        chart.bottom = statsTop - 90;
        if (chart.bottom < chart.top + Rpx(kChartHeight))
            chart.bottom = chart.top + Rpx(kChartHeight);
        // 上限 `100%` 在图表上方一行、右对齐到图表右缘；下限 `0` 在图表下方一行、
        // 左对齐到图表左缘。
        // 【UIFILE 声明】primaryCpuChart_bottom 是独立元素，含 RateLabelCpu(class=lheader) 与
        //   rheader content=33824「0」；上限来自 primaryCpuChart 的 title（S_duispec.md §3.4）。
        //   两者都在图表矩形**之外**，与本实现一致。
        //
        // 【P0-6 真实缺口】原版另有一个「显示内核时间」开关（MENU 30206 的
        //   CMD_SHOW_KERNEL_TIME），它要求在同一张图上**叠加第二条曲线**。
        //   采集层有数据（PerfSnapshot::cpuKernelPercent / CpuCoreSample::kernelTime），
        //   但 PerfHistory 只存了总利用率一条线，没有对应的历史副本。
        //   要真修需要：PerfHistory 加 cpuKernel[kPoints]、SetData 里 Push 第二份、
        //   这里再画一条曲线 + 图例。**本次没做**，菜单里那一项仍未接线 ——
        //   详见本次报告「未修 / 留给后续」，不在这里伪造一条假曲线。
        // 【已接线】「查看 → 显示内核时间」（CMD_SHOW_KERNEL_TIME）勾上时，
        //   在 CPU 主图上叠加第二条内核时间曲线。数据来自 PerfHistory::cpuKernel，
        //   由 SetData 每次采样写入，时间轴与 hist_.cpu 完全一致。
        //   此前这个勾选项只翻转 PerfToggles().kernelTime，绘制侧从不读它，
        //   是典型的「假功能」——勾上只多个对勾。
        PaintChart(hdc, chart, hist_.cpu, hist_.count, hist_.head,
                   L"100%", L"0", kChartLineCpu,
                   MenuBuilder::PerfToggles().kernelTime ? hist_.cpuKernel : nullptr);
    }

    // 统计行
    RECT stats{ rc.left + 8, statsTop, rc.right - 8, rc.bottom };
    PaintStatsRows(hdc, stats);
}

// CPU 页右上那张模式按钮的文字。
//
// 【UIFILE 无依据的既有实现】UIFILE 的 cpu_chart 段里没有这个下拉/expando 元素
// （原注释已记：只有 disk_chart / net_chart 有 dtb(ItemsView::Header,4,2)）。
// 这张按钮是既有的「切换入口」（HitTestChartToggle 命中它），所以保留。
// 但它既然是入口，文字就必须诚实反映当前 chart_ —— 以前恒写「总体利用率」，
// 用户点完「逻辑处理器」后按钮还是那句话，等于没有反馈。
//   - 逻辑处理器 / NUMA 节点：CPU 页专属视图，按钮显示模式名。
//   - 总体利用率：显示「总体利用率」。
//   - 单个引擎 / 多个引擎：GPU 页专属；SetChart 会把页面切到 GPU，
//     所以 CPU 页不会停留在这些模式上；万一停住（例如 GPU 页不可用），
//     也退回「总体利用率」，不谎称有引擎视图。
const wchar_t* PerfPage::CpuChartModeName() const {
    switch (chart_) {
    case ChartMode::Logical: return L"逻辑处理器";
    case ChartMode::Numa:    return L"NUMA 节点";
    default:                 return L"总体利用率";
    }
}

// 视图切换按钮的命中区域
// 必须与 PaintCpuPage 里画的矩形完全一致（rc.right-198..rc.right-7 /
// rc.top+30..rc.top+47，其中 rc 是 PaintContent 算出的 inner 矩形）。
// 【必须与 PaintContent 的 inner 同步】inner 左/右内缩都是 17rp（performancetab_tables padding）。
bool PerfPage::HitTestChartToggle(int x, int y) const {
    // 【P0-6】这个按钮只在 **CPU 页**绘制（PaintCpuPage 里那张模式按钮）。
    // 以前这里不判页面，于是磁盘/网络/GPU 页右上角同一坐标也会命中 ——
    // 用户在那三页点一下图表右上角，就会莫名其妙地被切回 CPU 页。
    // 既然注释要求「命中区必须与绘制矩形完全一致」，就必须同时判页面。
    //
    // 【本次改动】绘制侧现在**只在 Overall 视图**画这个按钮
    // （原版逻辑处理器网格视图右上角没有任何按钮，实测见 PaintCpuPage 注释），
    // 所以命中区也必须跟着收窄，否则网格视图里右上角会有一块看不见的点击热区。
    if (view_ != PerfView::Cpu) return false;
    if (chart_ != ChartMode::Overall) return false;
    RECT content{ bounds_.left + kSidebarWidth, bounds_.top,
                  bounds_.right, bounds_.bottom };
    RECT inner{ content.left + Rpx(kContentPadL), content.top + 12,
                content.right - Rpx(kContentPadL), content.bottom - 12 };
    RECT btn{ inner.right - 198, inner.top + 30, inner.right - 7, inner.top + 47 };
    return (x >= btn.left && x < btn.right && y >= btn.top && y < btn.bottom);
}

// 右侧属性列的宽度。内存/磁盘/网络/GPU 四页共用。
// 【为什么需要】UIFILE 30001 里 static_memory / static_disk / static_network /
// static_gpu 与各自的 dynamic_* 是**平级兄弟容器**，不是一个叠在另一个上面。
// 之前本文件把属性行直接画在图表矩形范围内（内存页 7 行、网络页 5 行、
// GPU 页 5 行），后画的把先画的盖掉 —— GPU 页甚至被图表的 FillRect 整块刷白。
// 现在统一给属性列留出右侧宽度，图表只占左边，两者不再相交。
// 【UIFILE 声明】atom(detailInfo) width = 402rp（docs/evidence/exec/_S_geo2_30001.tsv @0x51bc，
//   S_duispec.md §1.3 / §3.3）—— 这是右侧属性列的**声明宽度**。旧值 320 反解自
//   已证伪的 truth1.png，比声明值窄 82。
// 【UIFILE 声明】detailInfo padding = rect(0rp,10rp,24rp,20rp)：左 0 / 上 10 / 右 24 / 下 20。
namespace {
constexpr int kPropColW = 402;      // detailInfo width = 402rp
constexpr int kBarHeight = 52;      // BarchartMemoryHolder height = 52rp
}

void PerfPage::PaintMemPage(HDC hdc, const RECT& rc) const {
    // Win10 内存页结构（UIFILE 30001 的元素树，见 docs/specs/roast2/06_perf_mem_disk.md）：
    //   memory_chart
    //     memoryUsageTitle  → resstr(34360)「内存使用量」
    //     memory_size       ← 配套数值，格式串 resstr(34378)「%1!s!/%2!s! GB」
    //     memoryUsageGraph  ← **唯一一条**曲线
    //     memoryUsageGraph_bottom → RateLabelMem = resstr(33818)「0」
    //     memAvailabilityBar → BarTopLabel = resstr(34361)「内存组合」
    //   static_memory / dynamic_memory  ← 右侧属性，与 memory_chart 平级
    const auto& m = snap_ ? snap_->mem : MemBreakdown{};

    // 标题块：大标题「内存」，右上不再写「速度」。
    // 【MUI 佐证】速度（34349「速度:」）只出现在 static_memory 分组里，
    // memory_chart 与标题行没有速度元素。之前这里和属性区各画一次，重复了。
    PaintTitleBlock(hdc, rc, L"内存", L"", L"");

    // 图表只占左侧，右边留给属性列
    // 【UIFILE 声明】detailInfo width = 402rp（@0x51bc）
    const int propLeft = rc.right - Rpx(kPropColW);

    // ---- 内存使用量曲线 ----
    // 标题行（图表上方一行）：左边标题，右边 X/Y GB (Z%)。
    // 【MUI 佐证】resstr(34360)「内存使用量」是这张图的 title；
    // resstr(34378)「%1!s!/%2!s! GB」+ resstr(34384)「(%.0f%%)」是配套数值。
    // 之前这里画的是一个 22px 的裸百分比大数字 + 「已使用  X / Y」，
    // 两个格式串都是自创的（MUI 里没有「已使用」这个词，只有「已使用的插槽:」）。
    double usedPct = 0.0;
    if (m.totalBytes > 0) {
        usedPct = static_cast<double>(m.inUseBytes) /
                  static_cast<double>(m.totalBytes) * 100.0;
    }
    {
        wchar_t gb1[32], gb2[32];
        FmtBytesGB(m.inUseBytes, gb1, 32);
        FmtBytesGB(m.totalBytes, gb2, 32);

        // 图表矩形。
        // 【UIFILE 声明】memory_chart → chartArea 位置参数 rect(15rp,0rp,0rp,0rp)
        //   （_S_geo2_30001.tsv @0x5cd2）；主图 memoryUsageGraph 挂 sheet="staticResourceChart"
        //   → height=80rp（@0x320），layoutpos=client 可拉伸（S_duispec.md §5.8）。
        // 【UIFILE 声明】chartAndTitle padding = rect(0rp,0rp,0rp,10rp)（@0x51d6）——
        //   图表块与下方组合条之间声明了 10rp 的底部内边距。
        // 【UIFILE 未定义】右侧边界 propLeft-12 无 UIFILE 值，保留旧值。
        // 【图表高度 150 → 310】真原版截图实测 docs/reference/perf_mem_orig.png：
        //   图表矩形 y=152..462，**高 310px**（边框线实测 #8B12AE）。
        //   我方此前把高度硬限在 150，图表只有原版的一半高 —— 网格线因此只剩 3~4 条，
        //   即使间距改对也不像。改成 310 后 310/31 = 10 带，与原版的 8 条内部网格线一致。
        //   下限 rc.bottom-90 保留：窗口被压矮时不能盖住下方组合条。
        constexpr int kMemChartH = 310;   // 原版实测 152..462
        RECT chart{ rc.left + Rpx(kChartPad), rc.top + 56, propLeft - Rpx(12), 0 };
        chart.bottom = (std::min)(chart.top + kMemChartH, rc.bottom - 90);
        if (chart.bottom < chart.top + Rpx(kChartHeight)) chart.bottom = chart.top + Rpx(kChartHeight);

        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        RECT lbl{ chart.left, chart.top - 16, chart.left + 160, chart.top };
        DrawText2(hdc, L"内存使用量", &lbl, DT_SINGLELINE | DT_LEFT, kTitleText);
        {
            wchar_t v[96];
            swprintf_s(v, L"%s/%s (%.0f%%)", gb1, gb2, usedPct);
            RECT vr{ chart.right - 220, chart.top - 16, chart.right, chart.top };
            DrawText2(hdc, v, &vr, DT_SINGLELINE | DT_RIGHT, kTitleText);
        }
        SelectObject(hdc, old);

        // UIFILE 里 memoryUsageGraph **没有**独立的上限 title 元素 ——
        // 这一行只有左边的 memoryUsageTitle（「内存使用量」）和右边的 memory_size
        // （「X/Y GB (Z%)」）。所以 maxLabel 传 nullptr，不能像 CPU 页那样再挤一个 100%。
        PaintChart(hdc, chart, hist_.mem, hist_.count, hist_.head,
                   nullptr, L"0", kChartLineMem);

        // ---- 内存组合堆叠条（图表下方，横向占满内容区）----
        // 【UIFILE 声明】memAvailabilityBar：margin = rect(0rp,6rp,0rp,0rp) @0x5d52、
        //   padding = rect(0rp,0rp,0rp,14rp) @0x5d56；BarTopLabel（「内存组合」标题）
        //   padding = rect(0rp,0rp,0rp,5rp) @0x5d76；BarchartMemoryHolder height = 52rp @0x5a2。
        // +50 这个间距是旧实现为「0」标签 + 标题行留出的合成值，UIFILE 给的是 6+5+14，
        //   合成后约 25rp；但下方还要放「0」标签行，故保留 50 以免与轴标签重叠。
        const int barTop = chart.bottom + 50;
        {
            HFONT o2 = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
            SetBkMode(hdc, TRANSPARENT);
            RECT lbl{ rc.left + 8, barTop - 20, rc.left + 200, barTop - 4 };
            DrawText2(hdc, L"内存组合", &lbl, DT_SINGLELINE | DT_LEFT, kTitleText);
            SelectObject(hdc, o2);
        }

        if (m.totalBytes > 0 && barTop + Rpx(kBarHeight) < rc.bottom) {
            // 堆叠条属于 memory_chart（memAvailabilityBar），所以右沿跟图表一样
            // 停在属性列左侧，不能横穿到右侧属性区去。
            // 【UIFILE 声明】BarchartMemoryHolder height = 52rp（旧值 22 无出处）
            RECT bar{ rc.left + 8, barTop, propLeft - 12, barTop + Rpx(kBarHeight) };
            int w = bar.right - bar.left;

            // 四段配色：**主题给出了值，但与上屏不符，故沿用旧值**。
            // UIFILE 的 DashboardStyle 逐段引用（_S_refs.tsv / S_duispec.md §3.5）：
            //   BarchartInuse    content = gtc(TASKMANAGER,21,0,3802) = #F4F2F4
            //   BarchartModified content = gtc(TASKMANAGER,22,0,3802) = #EEE6F1，bordercolor = #CEB0D7 (part 23)
            //   BarchartStandby  content = gtc(TASKMANAGER,20,0,3802) = #FFFFFF，bordercolor = #CEB0D7
            //   BarchartFree     content = gtc(TASKMANAGER,20,0,3802) = #FFFFFF，bordercolor = #CEB0D7
            //   BarchartMemoryHolder bordercolor = gtc(CHARTVIEW,9,0,3802) = #8B12AE
            // 【为什么不用主题值】U_themecolors.md §5.3 明确记录：这四段主题值几乎是白，
            //   而原版上屏是「明显更深的紫」，两者无法统一；DUI 的 gtc 走的是专用 color scheme，
            //   OpenThemeData 拿到的是 NormalColor 默认映射，可能被上层覆盖。
            //   同时 wdc(dll) 的 barchart 字面量是另一套（绿/橙/蓝/浅蓝），也不适用于 taskmgr。
            //   三套候选互不相同、且没有一张可信的原版内存页截图可裁决，改任何一套都是猜。
            //   故保留现有配色，仅把「主题声明值」完整列在这里供后续裁决。
            struct Seg { uint64_t bytes; COLORREF color; const wchar_t* name; };
            uint64_t idleBytes = 0;
            if (m.totalBytes > m.inUseBytes + m.modifiedBytes + m.standbyBytes) {
                idleBytes = m.totalBytes - m.inUseBytes - m.modifiedBytes - m.standbyBytes;
            }
            Seg segs[] = {
                { m.inUseBytes,     RGB(0x0A, 0x3E, 0x70), L"使用中" },  // 33852；主题 21 = #F4F2F4
                { m.modifiedBytes,  RGB(0xC2, 0x5B, 0x36), L"已修改" },  // 33841；主题 22 = #EEE6F1
                { m.standbyBytes,   RGB(0x7F, 0xBA, 0xE0), L"备用" },    // 33853；主题 20 = #FFFFFF
                // 【MUI 佐证】最后一段原版逐字是「可用」= resstr(33854)，
                // 之前写的「空闲」在 MUI 里根本没有这个词。
                { idleBytes,        RGB(0xD8, 0xD8, 0xD8), L"可用" },    // 33854；主题 20 = #FFFFFF
            };
            // ↑ 四条：【主题实测 GetThemeColor，与上屏不符，沿用旧值】具体见上方说明

            int x = bar.left;
            int lx = bar.left;          // 图例横向游标
            const int ly = bar.bottom + 6;
            for (const auto& s : segs) {
                if (s.bytes == 0) continue;
                int sw = static_cast<int>(
                    static_cast<double>(s.bytes) / static_cast<double>(m.totalBytes) * w);
                if (sw < 1) sw = 1;
                if (x + sw > bar.right) sw = bar.right - x;
                if (sw <= 0) break;
                RECT sr{ x, bar.top, x + sw, bar.bottom };
                Fill(hdc, sr, s.color);
                x += sw;

                // 图例：色块 + 文字，横向并排
                if (ly + 16 < rc.bottom) {
                    RECT box{ lx, ly + 3, lx + 10, ly + 13 };
                    Fill(hdc, box, s.color);

                    HFONT old3 = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
                    SetBkMode(hdc, TRANSPARENT);
                    wchar_t gb[32], txt[96];
                    FmtBytesGB(s.bytes, gb, 32);
                    swprintf_s(txt, L"%s  %s", s.name, gb);
                    RECT lr{ lx + 14, ly, lx + 190, ly + 16 };
                    DrawText2(hdc, txt, &lr, DT_SINGLELINE | DT_LEFT, kSubText);

                    // 按文字实际宽度推进游标
                    SIZE sz{};
                    GetTextExtentPoint32W(hdc, txt, static_cast<int>(wcslen(txt)), &sz);
                    SelectObject(hdc, old3);
                    lx += 14 + sz.cx + 24;
                }
            }
        }
    }

    // ---- 右侧属性列 ----
    // 【UIFILE 声明】原版是两组（S_duispec.md §3.5，元素树）：
    //   static_memory（rowlayout(2)，5 组）：速度 / 已使用的插槽 / 外形规格 /
    //     为硬件保留的内存 / 最大内存
    //   dynamic_memory（6 组）：使用中(已压缩) / 可用 / 已提交 / 已缓存 /
    //     分页缓冲池 / 非分页缓冲池
    // 【UIFILE 声明】static_memory 每对 label/value 的 margin = rect(0rp,0rp,10rp,0rp)（12 处全同）；
    //   dynamic_memory 的 cpuSpeed/handle 类条目前有 margin rect(0rp,8rp,0rp,0rp)。
    // 【UIFILE 未定义】本实现的行高 17px、起始 y = rc.top+46 无 UIFILE 声明（UIFILE 用
    //   dynamicLabel 12px + dynamicData 21px 两行堆叠 + margin 间隔）。
    // 本实现只能取到其中 7 项 —— 已使用的插槽 / 外形规格 / 为硬件保留的内存 /
    // 最大内存这 4 项在 MemBreakdown（include/perfdata.h）里没有对应字段，
    // 不在本 agent 的文件范围内，故缺省不画（详见报告）。
    if (snap_) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        wchar_t line[128];
        int y = rc.top + 46;
        auto Put = [&](const wchar_t* s) {
            RECT r{ propLeft, y, rc.right, y + 17 };
            DrawText2(hdc, s, &r, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS, kSubText);
            y += 17;
        };
        wchar_t gb[32];
        // 34349「速度:」+ 34366「%u MHz」—— static_memory 的第一项
        swprintf_s(line, 128, L"速度:   %u MHz", snap_->cpuCurrentMhz); Put(line);
        // 【MUI 佐证】34399 逐字是「使用中(已压缩)」，之前漏了「(已压缩)」后缀
        FmtBytesGB(m.inUseBytes, gb, 32);
        swprintf_s(line, 128, L"使用中(已压缩):   %s", gb); Put(line);
        FmtBytesGB(m.availableBytes, gb, 32);
        swprintf_s(line, 128, L"可用:   %s", gb); Put(line);
        FmtBytesGB(m.committedBytes, gb, 32);
        wchar_t gb2[32];
        FmtBytesGB(m.commitLimitBytes, gb2, 32);
        swprintf_s(line, 128, L"已提交:   %s / %s", gb, gb2); Put(line);
        FmtBytesGB(m.cachedBytes, gb, 32);
        swprintf_s(line, 128, L"已缓存:   %s", gb); Put(line);
        FmtBytesGB(m.pagedPoolBytes, gb, 32);
        swprintf_s(line, 128, L"分页缓冲池:   %s", gb); Put(line);
        FmtBytesGB(m.nonPagedPoolBytes, gb, 32);
        swprintf_s(line, 128, L"非分页缓冲池:   %s", gb); Put(line);
        SelectObject(hdc, old);
    }
}

void PerfPage::PaintDiskPage(HDC hdc, const RECT& rc) const {
    // Win10 磁盘页（UIFILE 30001 的 disk_chart / disk_server_chart，S_duispec.md §3.6）：
    //   上：活动时间图  diskTimeTitle(34462「活动时间」) + diskTimeGraph(staticResourceChart)
    //       + diskTime_bottom → RateLabelDiskTime「0」
    //   下：磁盘传输速率图 diskRateGraph(staticResourceChart) + diskRate_bottom →
    //       RateLabelDiskRate「0」
    //   右侧：static_disk（rowlayout(3)）= 容量 / 已格式化 / 系统磁盘 / 页面文件 / 类型
    // 【UIFILE 声明】disk_chart 的 chartArea 位置参数是 rect(1rp,0rp,0rp,0rp)（紧贴组，
    //   _S_geo2_30001.tsv @0x61de / @0x628e）—— 与 CPU/内存页的 15rp 不同。
    // 【UIFILE 声明】两块之间 padding = rect(0rp,6rp,0rp,0rp)。
    const auto& disks = snap_ ? snap_->disks : std::vector<DiskSample>{};
    // 【P0-7】取**侧栏选中的那一块**磁盘，不是永远第 0 块。
    //   selInst_[Disk] 由 OnClick 从 items_[i].index 写入；快照刷新后数量可能变少，
    //   所以这里重新夹一次，越界就回退到 0，绝不越界读。
    //   上限取 disks.size() 与 hist_.diskCount 的较小者：标题里的盘名与曲线数据
    //   必须来自同一块盘，否则会出现「标题写磁盘 8、曲线画磁盘 0」。
    const int di = ClampInstance((std::min)(static_cast<int>(disks.size()),
                                            hist_.diskCount));
    const DiskSample* d = (di >= 0) ? &disks[di] : nullptr;

    // 标题：格式串 resstr(34450) = 「磁盘 %1!d! (%2!s!)」，第二槽是**盘符列表**，
    // 不是介质类型。介质类型（SSD/HDD）是独立元素 atom(sidebar_disk_type)。
    // 【证据缺口】DiskSample（include/perfdata.h）里既没有盘符列表也没有介质类型字段，
    // 采集层 src/perfdata.cpp 也把 name 写死成「磁盘 0」，不在本 agent 的文件范围内，
    // 所以这里暂时无法显示 `(D: E: C:)` 与 `SSD`。详见报告。
    // 【P0-7】标题用选中磁盘自己的名字 —— 以前恒为「磁盘 0」，
    //   点了第二块磁盘后标题仍然写「磁盘 0」，与曲线对不上。
    {
        wchar_t titleBuf[96];
        if (d && !d->name.empty()) {
            swprintf_s(titleBuf, L"%s", d->name.c_str());
        } else if (di >= 0) {
            swprintf_s(titleBuf, L"磁盘 %d", di);
        } else {
            wcscpy_s(titleBuf, L"磁盘");
        }
        PaintTitleBlock(hdc, rc, titleBuf, L"", L"");
    }

    const int propLeft = rc.right - Rpx(kPropColW);

    // 图表可用高度。【UIFILE 未定义】两图均分 + 80 的让位是既有自适应算法，保留。
    int top = rc.top + 46;
    int bottom = rc.bottom - 8;
    int availH = bottom - top;
    int chartH = (availH - 80) / 2;   // 80 = 上图「0」标签 15 + 间距 40 + 下图标题 16 + 下图「0」15
    if (chartH < 50) chartH = 50;

    // ---- 上：活动时间 ----
    {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        RECT lbl{ rc.left + Rpx(kChartPadTight), top - 16, rc.left + 200, top };
        DrawText2(hdc, L"活动时间", &lbl, DT_SINGLELINE | DT_LEFT, kTitleText);
        SelectObject(hdc, old);
    }
    RECT c1{ rc.left + Rpx(kChartPadTight), top, propLeft - Rpx(12), top + chartH };
    // 【P0-7】活动时间曲线用**选中那块盘**的逐盘历史；没有实例时退回聚合曲线。
    if (di >= 0 && di < hist_.diskCount) {
        PaintChart(hdc, c1, hist_.diskActiveI[di], hist_.count, hist_.head,
                   L"100%", L"0", kChartLineDisk);
    } else {
        PaintChart(hdc, c1, hist_.disk, hist_.count, hist_.head,
                   L"100%", L"0", kChartLineDisk);
    }

    // ---- 下：磁盘传输速率 ----
    // 间距 40 = 上图底边 + 15px 的「0」标签行 + 9px 空 + 16px 的「磁盘传输速率」标题行。
    // 上图的下限「0」画在 c1.bottom..c1.bottom+15，下图的标题画在
    // top2-16..top2 → 两者必须完全不重叠，所以 15+16=31 是硬下限，取 40 留余量。
    int top2 = c1.bottom + 40;
    {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        RECT lbl{ rc.left + Rpx(kChartPadTight), top2 - 16, rc.left + 240, top2 };
        DrawText2(hdc, L"磁盘传输速率", &lbl, DT_SINGLELINE | DT_LEFT, kTitleText);
        SelectObject(hdc, old);
    }
    RECT c2{ rc.left + Rpx(kChartPadTight), top2, propLeft - Rpx(12),
             (std::min)(top2 + chartH, bottom - 18) };   // 底部留出 `0` 标签的位置

    // 量程：从 MUI 的固定档位表里挑一个刚好包住当前数据的档位。
    // 【MUI 佐证】32840-32849 是 100 KB/秒 / 500 KB/秒 / 1 MB/秒 / 10 / 100 / 250 /
    // 500 MB/秒 / 1 / 10 / 100 GB/秒 这一套十进制阶梯，32850-32858 是补充档位。
    // 原版是「从这个表里挑档」，不是「取本窗口最大值」—— 后者会让峰值恒等于满格，
    // 曲线每一帧都贴着顶边，用户完全看不出磁盘闲不闲。
    {
        // 档位单位统一换算成 MB/s
        struct Tier { double mbps; const wchar_t* label; };
        const Tier tiers[] = {
            { 100.0 / 1024,        L"100 KB/秒" },
            { 500.0 / 1024,        L"500 KB/秒" },
            { 1.0,                 L"1 MB/秒" },
            { 10.0,                L"10 MB/秒" },
            { 100.0,               L"100 MB/秒" },
            { 250.0,               L"250 MB/秒" },
            { 500.0,               L"500 MB/秒" },
            { 1024.0,              L"1 GB/秒" },
        };
        const int nTiers = static_cast<int>(sizeof(tiers) / sizeof(tiers[0]));

        // 【P0-7】速率历史按选中的那块盘取；没有实例时退回聚合活动时间曲线
        // （量纲同为 0..100，档位表会挑到最小档，不会画出凭空的 GB/s）。
        const bool haveInst = (di >= 0 && di < hist_.diskCount);
        const double* rateSrc = haveInst ? hist_.diskRateI[di] : hist_.disk;

        double mx = 0.0;
        for (int i = 0; i < hist_.count; ++i) {
            double v = hist_.At(rateSrc, i);
            if (v > mx) mx = v;
        }
        int sel = nTiers - 1;
        for (int i = 0; i < nTiers; ++i) {
            if (mx <= tiers[i].mbps * 0.95) { sel = i; break; }
        }
        if (mx > tiers[nTiers - 1].mbps * 0.95) sel = nTiers - 1;
        const double limit = tiers[sel].mbps;

        double ordered[PerfHistory::kPoints];
        for (int i = 0; i < PerfHistory::kPoints; ++i) {
            int idx = (hist_.head - hist_.count + i + PerfHistory::kPoints * 2)
                      % PerfHistory::kPoints;
            double v = (i < hist_.count) ? rateSrc[idx] : 0.0;
            double p = v / limit * 100.0;
            if (p > 100.0) p = 100.0;
            ordered[i] = p;
        }
        PaintChart(hdc, c2, ordered, hist_.count, hist_.count,
                   tiers[sel].label, L"0", kChartLineDisk);
    }

    // ---- 右侧磁盘属性（static_disk，标签左对齐 / 值右对齐）----
    // 【MUI + UIFILE 声明】原版 5 项且顺序固定（static_disk rowlayout(3)）：
    //   容量:(34453) / 已格式化:(34454) / 系统磁盘:(34455) / 页面文件:(34456) / 类型:(34451)
    // 之前画的「读取速度 / 写入速度 / 平均响应时间 / 传输速率」是
    // dynamic_disk_server 的**表格列**，不是这一屏的右侧属性 —— 整组撤掉。
    // 【UIFILE 未定义】标签列宽 110px、行高 17px 无 UIFILE 声明，保留旧值。
    //
    // 【证据缺口】DiskSample（include/perfdata.h:53-59）只有
    // name / activePercent / readMBps / writeMBps / avgResponseMs 五个字段，
    // 没有容量、已格式化、系统磁盘、页面文件、介质类型中的任何一个，
    // 而采集层在 src/perfdata.cpp:596-641，不在本 agent 允许改的文件范围内。
    // 这里按原版文案把五行**标签**画出来（标签是硬证据），值统一用「—」占位，
    // 等采集层补字段后填真值。绝不编造数字。
    if (d) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        int y = rc.top + 46;
        auto Put = [&](const wchar_t* lab, const wchar_t* val) {
            RECT lr{ propLeft, y, propLeft + 110, y + 17 };
            DrawText2(hdc, lab, &lr, DT_SINGLELINE | DT_LEFT, kSubText);
            RECT vr{ propLeft + 110, y, rc.right, y + 17 };
            DrawText2(hdc, val, &vr, DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS, kSubText);
            y += 17;
        };
        // 五项现在都有真数据源（docs/audit/A14 §6.1）：
        //   容量 / 已格式化  ← IOCTL_DISK_GET_LENGTH_INFO + GetDiskFreeSpaceExW
        //   系统磁盘         ← 比对 %SystemDrive%
        //   页面文件         ← 盘根下是否有 pagefile.sys
        //   类型             ← STORAGE_DEVICE_DESCRIPTOR.BusType
        wchar_t b[64];

        auto FmtCap = [](uint64_t bytes, wchar_t* out2, size_t n2) {
            double gb = static_cast<double>(bytes) / 1073741824.0;
            if (gb >= 1000.0) swprintf_s(out2, n2, L"%.1f TB", gb / 1024.0);
            else              swprintf_s(out2, n2, L"%.1f GB", gb);
        };

        if (d->capacityBytes) {
            FmtCap(d->capacityBytes, b, 64);
            Put(L"容量:", b);
        } else {
            Put(L"容量:", L"—");
        }

        if (d->freeBytes) {
            FmtCap(d->freeBytes, b, 64);
            Put(L"已格式化:", b);
        } else {
            Put(L"已格式化:", L"—");
        }

        Put(L"系统磁盘:", d->isSystemDisk ? L"是" : L"否");
        Put(L"页面文件:", d->hasPageFile ? L"是" : L"否");

        if (!d->mediaType.empty() && !d->busType.empty()) {
            swprintf_s(b, L"%ls (%ls)", d->mediaType.c_str(), d->busType.c_str());
            Put(L"类型:", b);
        } else if (!d->mediaType.empty()) {
            Put(L"类型:", d->mediaType.c_str());
        } else {
            Put(L"类型:", L"—");
        }
        SelectObject(hdc, old);
    }
}

void PerfPage::PaintNetPage(HDC hdc, const RECT& rc) const {
    // Win10 网络页（UIFILE 30001 的 net_chart，S_duispec.md §3.6）：
    //   netChartTitle(34560) / netChartMax / net_adapter
    //   chartArea → 主曲线(sheet=staticResourceChart) + primaryNetChart_bottom →
    //               RateLabelNet「0」
    //   static_network（rowlayout(4)，8 组）：适配器名称 / 服务提供商 / SSID / DNS 名称 /
    //               连接类型 / IPv4 / IPv6 / 信号强度
    // 【UIFILE 声明】net_chart 的 chartArea 位置参数是 rect(1rp,0rp,0rp,0rp)（紧贴组）
    //
    // 选哪个适配器。
    // 【P0-7】以前这里按「谁当前流量最大」挑，完全无视侧栏的选中项 ——
    //   用户点「以太网」高亮，图上却可能是「WLAN」（正好最忙的那个）。
    //   现在优先用侧栏选中的实例；选不中（实例数变少 / 从菜单直接切到本页）
    //   才退回「最忙的那个」，避免显示一块永远 0 的虚拟网卡。
    const auto& nets = snap_ ? snap_->nets : std::vector<NetSample>{};
    // 上限同时受 hist_.netCount 约束：曲线要取 hist_.netSentI[ni]，
    // 而那份逐实例历史只存了前 kMaxInstances 个（SetData 里截过）。
    // 不夹住的话，网卡数 > kMaxInstances 时会出现「标题是第 12 块网卡、
    // 曲线读的是越界下标」。
    const int ni = ClampInstance((std::min)(static_cast<int>(nets.size()),
                                            hist_.netCount));
    const NetSample* n = (ni >= 0) ? &nets[ni] : nullptr;
    if (!n) {
        double bestScore = -1.0;
        for (const auto& c : nets) {
            double s = (std::max)(c.sendMbps, c.recvMbps);
            if (s > bestScore) { bestScore = s; n = &c; }
        }
    }
    if (!n && !nets.empty()) n = &nets[0];

    // 标题：适配器显示名（UIFILE 元素 net_adapter）。
    // 右上角不再写「x.x Mbps」—— UIFILE 的 dynamic_network 里没有「标题右上速率文本」
    // 对应的 resstr（只有 netChartTitle / netChartMax / net_adapter / RateLabelNet），
    // 速率给了 netChartMax（即图表量程上限），已由 PaintChart 的 maxLabel 负责。
    wchar_t titleBuf[128];
    if (n && !n->name.empty()) {
        swprintf_s(titleBuf, L"%s", n->name.c_str());
    } else {
        wcscpy_s(titleBuf, L"以太网");
    }
    PaintTitleBlock(hdc, rc, titleBuf, L"", L"");

    const int propLeft = rc.right - Rpx(kPropColW);
    int top = rc.top + 46;
    int bottom = rc.bottom - 8;

    // 吞吐量曲线（只占左侧，右侧留给属性列）
    // 【UIFILE 声明】chartArea 位置参数 rect(1rp,0rp,0rp,0rp)（@0x6756，紧贴组）
    // 【UIFILE 声明】主曲线 sheet=staticResourceChart → height 基准 80rp（@0x320）
    // 【UIFILE 未定义】图上方的 `吞吐量` 标题行 20px、「上界 150」与右侧 −12 为既有实现。
    RECT chart{ rc.left + Rpx(kChartPadTight), top + 20, propLeft - Rpx(12),
                (std::min)(top + 20 + 150, bottom) };

    {
        // 【P0-7】曲线数据源改成**选中适配器自己的**收发历史（原始 Mbps）。
        //   以前读 hist_.net —— 那是全机「最忙网卡」的合计，写成「占 1 Gbps 的百分比」，
        //   于是既不对应选中的网卡，单位还被换算来换算去（下面那 10 倍换算是旧错误）。
        //   现在 netSentI/netRecvI 存的就是 Mbps，直接按档位归一。
        const bool haveInst = (ni >= 0 && ni < hist_.netCount);
        double mx = 0.0;
        double ordered[PerfHistory::kPoints];

        // 逐点值（Mbps）：选中实例取收发较大者；无实例时退回聚合曲线换算。
        auto sampleAt = [&](int k) -> double {
            if (haveInst) {
                return (std::max)(hist_.At(hist_.netSentI[ni], k),
                                  hist_.At(hist_.netRecvI[ni], k));
            }
            // 聚合兜底：hist_.net 是「占 1 Gbps 的百分比」，×10 得 Mbps
            return hist_.At(hist_.net, k) * 10.0;
        };
        for (int i = 0; i < hist_.count; ++i) {
            double v = sampleAt(i);
            if (v > mx) mx = v;
        }
        double mxMbps = mx;

        // 【MUI 佐证】原版网络图的单位随量级切换，单位串是
        // 32875 bps / 32876 Kbps / 32877 Mbps / 32878 Gbps。
        // 用十进阶梯挑档：1 / 10 / 100 / 1000 Mbps
        double limit = 10.0;
        const wchar_t* limitLabel = L"10 Mbps";
        if (mxMbps <= 1.0)   { limit = 1.0;    limitLabel = L"1 Mbps"; }
        else if (mxMbps <= 10.0)  { limit = 10.0;   limitLabel = L"10 Mbps"; }
        else if (mxMbps <= 100.0) { limit = 100.0;  limitLabel = L"100 Mbps"; }
        else                      { limit = 1000.0; limitLabel = L"1 Gbps"; }

        for (int i = 0; i < PerfHistory::kPoints; ++i) {
            double v = (i < hist_.count) ? sampleAt(i) : 0.0;
            double p = v / limit * 100.0;
            if (p > 100.0) p = 100.0;
            ordered[i] = p;
        }
        PaintChart(hdc, chart, ordered, hist_.count, hist_.count,
                   limitLabel, L"0", kChartLineNet);
    }

    // 吞吐量曲线的标题行：`吞吐量` 在左、`<量程>` 在右 —— 同一行，都在图表上方。
    // 所以图表起始必须让出这一行：title 占 top..top+16，图表从 top+20 起，
    // 而 PaintChart 的上限标签画在 chart.top-16 = top+4 —— 与标题同一行，不重叠。
    // 图例已撤销：【mun 二进制佐证，非截图】#B15C11 / #DC652B 是原版 mun 里
    // IDB_PERFTAB_NET_RECEIVE_COLORKEY / IDB_PERFTAB_NET_SEND_COLORKEY 的实测单色，
    // 也就是说它们是**曲线本身的颜色**，不是图例色卡。【UIFILE 声明】原版吞吐量图是
    // 收/发双色叠加（dynamic_network 的 netSendLegend / netReceiveLegend 两组元素），
    // 我方只画一条 #A74F01 的合计曲线 —— 摆两个图里不存在的色块就是图例说谎。
    // 本实现拿不到分向历史（netproc.cpp 只给合计，不在本 agent 范围），
    // 所以撤掉图例，不留「承诺两条线、实际画一条」的假。
    // 收发数值本身仍在侧栏卡片里显示。
    {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        RECT lbl{ rc.left + Rpx(kChartPadTight), top, rc.left + 90, top + 16 };
        DrawText2(hdc, L"吞吐量", &lbl, DT_SINGLELINE | DT_LEFT, kTitleText);
        SelectObject(hdc, old);
    }

    // 右侧属性（static_network）。
    // 【UIFILE 声明】原版是 8 组成对 label+value（rowlayout(4)，S_duispec.md §3.6）：
    //   适配器名称(34549) / 服务提供商(34550) / SSID(34558) / DNS 名称(34554) /
    //   连接类型(34551) / IPv4 地址(34552) / IPv6 地址(34553) / 信号强度(34564)
    // 【UIFILE 未定义】标签列宽 100px、行高 17px、起始 y=rc.top+46 无 UIFILE 声明（既有实现）。
    // 【证据缺口】NetSample（include/perfdata.h:61-67）只有 name / sendMbps /
    // recvMbps / linkSpeedBps —— 没有 IP、DNS、SSID、服务提供商、信号强度、连接类型。
    // 采集层在 src/perfdata.cpp:689-742，不在本 agent 的文件范围内。
    // 这里按原版文案与顺序把 8 个标签全画出来，取不到的填「—」，绝不编造；
    // 同时把「接收」「发送」「链接速度」从属性列撤掉 —— 这三项在原版
    // static_network 里不存在（发送/接收属于左侧卡片，链接速度属于详细子表）。
    if (n) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);
        int y = rc.top + 46;
        auto Put = [&](const wchar_t* lab, const wchar_t* val) {
            RECT lr{ propLeft, y, propLeft + 100, y + 17 };
            DrawText2(hdc, lab, &lr, DT_SINGLELINE | DT_LEFT, kSubText);
            RECT vr{ propLeft + 100, y, rc.right, y + 17 };
            DrawText2(hdc, val, &vr, DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS, kSubText);
            y += 17;
        };
        Put(L"适配器名称:", n->name.empty() ? L"—" : n->name.c_str());
        Put(L"服务提供商:", L"—");
        Put(L"SSID:", L"—");
        Put(L"DNS 名称:", L"—");
        // 【已知硬编码】NetSample 没有 type 字段，之前固定写「以太网」。
        // 原版取值表 34693 以太网 / 34694 Wi-Fi / 34695 移动设备 /
        // 34696 Wi-Fi Direct / 34697 WLAN Direct。这里保留「以太网」但标注清楚，
        // 因为替换成任何猜测值都更差。
        Put(L"连接类型:", L"以太网");
        Put(L"IPv4 地址:", L"—");
        Put(L"IPv6 地址:", L"—");
        Put(L"信号强度:", L"—");
        SelectObject(hdc, old);
    }
}

void PerfPage::PaintGpuPage(HDC hdc, const RECT& rc) const {
    // Win10 GPU 页的元素树（UIFILE 30001 的 gpu_chart，S_duispec.md §3.7）：
    //   gpuPrimaryCharts / gpuMemoryCharts(margin rect(0rp,5rp,0rp,5rp)) /
    //   singleEngineGpuView(topArea / engineSelector / engineTitle / engineUtilizationRate /
    //                       chartArea[rect(15rp,0,0,0)] → engineChart sheet=gpuchart) /
    //   multiEngineGpuView(gpuGrid → gpuEngineChartTemplate padding rect(2rp,2rp,2rp,2rp))
    //   static_gpu（rowlayout(5)）：驱动版本 41001 / 日期 41002 / DX 版本 41003 /
    //             物理位置 41032 / 硬件保留 41033
    //   dynamic_gpu：gpuUsage / gpuTotalMemory / gpuDedicatedMemRoot / gpuSharedMemRoot / gpuTemperature
    // 【UIFILE 声明】gpuchart / gpuEngineChartTemplate 的尺寸：80rp × 80rp、padding rect(2rp,2rp,2rp,2rp)
    //   引擎名（3D / Copy / Video Encode / Video Decode）来自 taskmgr.exe 的连续字符串表。

    // 【多卡】取**当前选中那张卡**的适配器信息，不是永远第一张。
    // gpuAdapters 与 gpus 同下标（perfdata.cpp 里同时填充），所以用同一个 gi。
    // 拿不到时退回 gpuAdapter（= 第一张卡）保持旧行为。
    const int giSel0 = snap_ ? ClampInstance((std::min)(static_cast<int>(snap_->gpus.size()),
                                                       hist_.gpuCount)) : -1;
    static const GpuAdapterInfo kEmptyAdapter{};
    const GpuAdapterInfo& a =
        (snap_ && giSel0 >= 0 && giSel0 < static_cast<int>(snap_->gpuAdapters.size()))
            ? snap_->gpuAdapters[giSel0]
            : (snap_ ? snap_->gpuAdapter : kEmptyAdapter);

    // 标题：`GPU N` + 适配器名。
    // 右上「利用率: X%」撤掉 —— UIFILE 的 dynamic_gpu 里没有对应元素。
    // 【P0-7】GPU 序号跟着侧栏选中项走，不再恒写 `GPU 0`。
    // 【多卡修复】适配器名必须取该实例自己的（snap_->gpuAdapters[gi].name）。
    // 旧代码优先用 snap_->gpus[gi].name —— 那个字段现在是 "GPU 0"/"GPU 1" 的
    // 卡片标题，不再装适配器名，于是副标题会重复成 "GPU 0"。
    const int gi = giSel0;
    {
        wchar_t titleBuf[64];
        swprintf_s(titleBuf, L"GPU %d", gi >= 0 ? gi : 0);
        const wchar_t* sub = L"";
        if (!a.name.empty()) sub = a.name.c_str();
        PaintTitleBlock(hdc, rc, titleBuf, sub, L"");
    }

    // 右侧静态信息列（static_gpu，rowlayout(5)）。
    //
    // 【严重修复，保留】之前属性 5 行从 rc.top+40 起（行距 17 → 占到 rc.top+125），
    // 而总计使用率曲线从 rc.top+90 起、且 PaintChart 第一件事就是 Fill(rc, kChartBg)，
    // **属性第 4、5 行（物理位置 / 为硬件保留的内存）被图表底色整块刷掉，一个字都看不见**。
    // 现在给属性列划出独立的右侧宽度（detailInfo 声明宽 402rp），图表只占左边，两者不再相交。
    // 【UIFILE 未定义】标签列宽 120px、行高 17px、起始 y=rc.top+46 无 UIFILE 声明。
    const int gpuPropLeft = rc.right - Rpx(kPropColW);
    {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);

        auto DateText = [&]() -> const wchar_t* {
            return a.driverDate.empty() ? L"—" : a.driverDate.c_str();
        };
        // 【MUI 佐证】41032「物理位置：」、41033「为硬件保留的内存：」，
        // 同一张表里其它标签（41000 适配器名称: / 41001 驱动程序版本: /
        // 41002 驱动程序日期: / 41003 DirectX 版本:）都是**半角**冒号，
        // 只有这两个是全角「：」。之前 5 行统一用了半角。
        // 同时按原版改成「标签左对齐 + 值右对齐」的两端排布（之前是整串左对齐）。
        int y = rc.top + 46;
        auto Put = [&](const wchar_t* lab, const wchar_t* val) {
            RECT lr{ gpuPropLeft, y, gpuPropLeft + 120, y + 17 };
            DrawText2(hdc, lab, &lr, DT_SINGLELINE | DT_LEFT, kSubText);
            RECT vr{ gpuPropLeft + 120, y, rc.right, y + 17 };
            DrawText2(hdc, val, &vr, DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS, kSubText);
            y += 17;
        };

        Put(L"驱动程序版本:",
            a.driverVersion.empty() ? L"—" : a.driverVersion.c_str());
        Put(L"驱动程序日期:", DateText());
        Put(L"DirectX 版本:",
            a.directXVersion.empty() ? L"—" : a.directXVersion.c_str());
        Put(L"物理位置：",
            a.physicalLocation.empty() ? L"—" : a.physicalLocation.c_str());
        if (a.hwReserved > 0) {
            wchar_t gb[32];
            FmtBytesGB(a.hwReserved, gb, 32);
            Put(L"为硬件保留的内存：", gb);
        } else {
            Put(L"为硬件保留的内存：", L"—");
        }
        SelectObject(hdc, old);
    }

    // ---- 总计使用率曲线（只占左侧）----
    // 【UIFILE 声明】GPU 页 chartArea 位置参数 rect(15rp,0rp,0rp,0rp)（@0x6b16，宽松组）；
    //   单引擎图 engineChart 挂 sheet=gpuchart → 80rp × 80rp、padding rect(2rp,2rp,2rp,2rp)。
    // 【UIFILE 未定义】上界 130 / 下界 −130 / 顶部偏移 56 为既有自适应，保留。
    RECT chart{ rc.left + Rpx(kChartPad), rc.top + 56, gpuPropLeft - Rpx(12),
                (std::min)(rc.top + 56 + 130, rc.bottom - 130) };
    if (chart.bottom < chart.top + Rpx(kChartHeight)) chart.bottom = chart.top + Rpx(kChartHeight);
    // 【P0-6】GPU 页也接上「将图形更改为」：
    //   Overall（默认）→ 总利用率曲线 + 显存两块 + 4 个引擎小图
    //   SingleEngine  → 把占用最高的那个引擎放大成一整块（不再画总曲线/显存/其它引擎）
    //   MultiEngine   → 4 个引擎占满整页（不再画总曲线/显存）
    //   Logical / Numa（CPU 页专用）→ 与 Overall 相同，GPU 页没有对应语义
    const bool singleEng = (chart_ == ChartMode::SingleEngine);
    const bool multiEng  = (chart_ == ChartMode::MultiEngine);

    // ---- 引擎格子定义（下面两处布局共用）----
    // 只显示 Win10 有显示名的引擎类型：3D / Copy / Video Encode / Video Decode
    // （引擎名来自 taskmgr.exe 的连续字符串表）
    // 【UIFILE 声明】multiEngineGpuView → gpuGrid → gpuEngineChartTemplate → gpuEngineChart
    //   挂 sheet=gpuchart：height = 80rp、width = 80rp、padding = rect(2rp,2rp,2rp,2rp)
    //   （_S_geo2_30001.tsv @0x1c4..@0x1cc / 30024 @0x1f8..@0x200）。
    //   原版是多引擎**平铺**（gpuGrid），本实现固定 2×2。
    struct EngineSlot { int type; const wchar_t* label; };
    const EngineSlot wanted[] = {
        { 1, L"3D" },            // DXGK_ENGINE_TYPE_3D
        { 6, L"Copy" },          // _COPY
        { 3, L"Video Encode" },  // _VIDEO_ENCODE
        { 2, L"Video Decode" },  // _VIDEO_DECODE
    };
    const int nSlots = static_cast<int>(sizeof(wanted) / sizeof(wanted[0]));
    const int chartBottomLimit = rc.bottom - 6;

    // 【多卡】本页绘制哪一个实例的引擎数据。
    // 传 -1 表示"用合并视图"（没有实例概念的历史路径）。
    const int engInst = (gi >= 0 && gi < hist_.gpuCount) ? gi : -1;

    // ---- 单个引擎视图：整块留给占用最高的那个引擎 ----
    if (singleEng) {
        // 选哪个引擎：当前占用最高的。全部为 0 时退回 3D（枚举里 3D 永远是第一格），
        // 这样单引擎视图在任何机器上都有内容，不会是一块空框。
        int bestSlot = 0;
        double bestUtil = -1.0;
        for (int i = 0; i < nSlots; ++i) {
            double u = 0.0;
            if (snap_) {
                // 【多卡】只看**当前选中的那张卡**的引擎，否则单引擎视图可能
                // 放大的是另一张卡的引擎。
                const std::vector<GpuEngineSample>* list = &snap_->gpuEngines;
                if (gi >= 0 && gi < static_cast<int>(snap_->gpuAdapters.size()))
                    list = &snap_->gpuAdapters[gi].engines;
                for (const auto& e : *list) {
                    if (e.engineType == wanted[i].type) { u = e.utilPercent; break; }
                }
            }
            if (u > bestUtil) { bestUtil = u; bestSlot = i; }
        }
        RECT big{ rc.left + Rpx(kChartPad), rc.top + 56,
                  gpuPropLeft - Rpx(12), chartBottomLimit };
        if (big.bottom > big.top + 60) {
            PaintEngineChart(hdc, big, wanted[bestSlot].type, wanted[bestSlot].label,
                             engInst);
        }
        return;
    }

    PaintChart(hdc, chart,
               (gi >= 0 && gi < hist_.gpuCount) ? hist_.gpuI[gi] : hist_.gpu,
               hist_.count, hist_.head,
               L"100%", L"0", kChartLineGpu);

    // ---- 专用 / 共享 GPU 内存 ----
    // 【多卡已接线】a 是**当前选中那张卡**的 GpuAdapterInfo（见本函数开头），
    //   所以显存数字跟着侧栏选中的 GPU 卡片走，不再永远显示第一张卡。
    // 【UIFILE 声明】Win10 在曲线下方有两个数字块：gpuDedicatedMemRoot / gpuSharedMemRoot
    //   （dynamic_gpu 段），两者在 UIFILE 里 `0x802a` 参数都是 rect(1rp,0rp,0rp,0rp)，
    //   没有 height —— 高度由内容撑出。gpuMemoryCharts 的 margin = rect(0rp,5rp,0rp,5rp)（@0x6b26）。
    // 【UIFILE 未定义】+18 的间距、两列各 220px 宽、标签 16px / 数值 34px 的纵向偏移为既有实现。
    // 【P0-6】「多个引擎」视图下不画这两块 —— 那个视图按原版是满屏引擎网格（gpuGrid）。
    int memTop = chart.bottom + 18;
    if (!multiEng && memTop + 44 < rc.bottom) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
        SetBkMode(hdc, TRANSPARENT);

        wchar_t gb[32];
        FmtBytesGB(a.dedicatedInUse, gb, 32);
        RECT l1{ rc.left + 8, memTop, rc.left + 220, memTop + 16 };
        DrawText2(hdc, L"专用 GPU 内存", &l1, DT_SINGLELINE | DT_LEFT, kTitleText);
        SelectObject(hdc, fontBold_);
        RECT v1{ rc.left + 8, memTop + 16, rc.left + 220, memTop + 34 };
        DrawText2(hdc, gb, &v1, DT_SINGLELINE | DT_LEFT, kTitleText);

        SelectObject(hdc, fontSmall_);
        FmtBytesGB(a.sharedInUse, gb, 32);
        int midX = rc.left + (rc.right - rc.left) / 2;
        RECT l2{ midX, memTop, midX + 220, memTop + 16 };
        DrawText2(hdc, L"共享 GPU 内存", &l2, DT_SINGLELINE | DT_LEFT, kTitleText);
        SelectObject(hdc, fontBold_);
        RECT v2{ midX, memTop + 16, midX + 220, memTop + 34 };
        DrawText2(hdc, gb, &v2, DT_SINGLELINE | DT_LEFT, kTitleText);

        SelectObject(hdc, old);
    }

    // ---- 4 个引擎小图 ----
    // 【P0-6】「多个引擎」视图：整页 2×2 铺满（原版 multiEngineGpuView 的 gpuGrid 语义）。
    //   默认/总体视图：总曲线 + 显存两块之后的下半部分。
    // 【UIFILE 未定义】2 列布局、8px 缝隙、130 的高度上限为既有实现。
    int engTop = multiEng ? (rc.top + 56) : (memTop + 46);
    int engBottom = chartBottomLimit;
    int availH = engBottom - engTop;
    // 两行四格；每格高度按可用空间均分，保证填满而不是挤在上半部
    int engH = (availH - 8) / 2;
    if (engH > 130) engH = 130;
    // 【UIFILE 声明】gpuchart 的声明高是 80rp；但这是**风格基准**，窗口被压矮时
    //   必须允许缩小，否则下面的 `engTop + engH <= rc.bottom` 会整块不画（空白页）。
    //   【无依据，沿用旧值】46 的下限是既有防御值，保留。
    if (engH < 46) engH = 46;

    if (engTop + engH <= rc.bottom) {
        const int gapE = 8;
        int colW = (rc.right - 8 - (rc.left + 8) - gapE) / 2;
        for (int i = 0; i < nSlots; ++i) {
            int cx = rc.left + 8 + (i % 2) * (colW + gapE);
            int cy = engTop + (i / 2) * (engH + 8);
            RECT slot{ cx, cy, cx + colW, cy + engH };
            if (slot.bottom > rc.bottom) break;
            PaintEngineChart(hdc, slot, wanted[i].type, wanted[i].label, engInst);
        }
    }
}

// 单个 GPU 引擎的小图。Win10 的「多个引擎」视图里每格就是这个结构：
// 左上角引擎名，右侧当前百分比，下面一条**历史面积图**（不是一条水平线）。
// inst >= 0 时画该实例自己的引擎历史（hist_.gpuEngineI[inst]），
// inst < 0 时回退到合并视图（hist_.gpuEngine）。
void PerfPage::PaintEngineChart(HDC hdc, const RECT& rc, int engineType,
                               const wchar_t* label, int inst) const {
    // 底色与主图一致改成纯白：同一页上主图已是 #FFFFFF（原版实测），
    // 引擎格若还是 #F1F6FA 会出现两块不同底色。
    Fill(hdc, rc, RGB(0xFF, 0xFF, 0xFF));

    // 找这个引擎的当前值
    double util = 0.0;
    bool found = false;
    if (snap_) {
        // 【多卡】inst 有效时只查该实例自己的引擎列表
        const std::vector<GpuEngineSample>* list = &snap_->gpuEngines;
        if (inst >= 0 && inst < static_cast<int>(snap_->gpuAdapters.size()))
            list = &snap_->gpuAdapters[inst].engines;
        for (const auto& e : *list) {
            if (e.engineType == engineType) { util = e.utilPercent; found = true; break; }
        }
    }

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontSmall_));
    SetBkMode(hdc, TRANSPARENT);

    // 标题 + 百分比
    RECT lr{ rc.left + 6, rc.top + 3, rc.right - 60, rc.top + 18 };
    DrawText2(hdc, label, &lr, DT_SINGLELINE | DT_LEFT, kTitleText);

    wchar_t pct[32];
    if (found) swprintf_s(pct, L"%.0f%%", util);
    else       swprintf_s(pct, L"——");
    RECT pr{ rc.right - 56, rc.top + 3, rc.right - 6, rc.top + 18 };
    DrawText2(hdc, pct, &pr, DT_SINGLELINE | DT_RIGHT, kSubText);

    // ---- 历史面积图 ----
    // 【修复】之前只按当前值画一条水平线，看不出变化趋势。
    // 现在画真实的逐引擎历史曲线，量程自适应（否则 3% 的引擎看起来是平的）。
    RECT plot{ rc.left + 6, rc.top + 22, rc.right - 6, rc.bottom - 4 };
    int pw = plot.right - plot.left;
    int ph = plot.bottom - plot.top;
    if (pw > 8 && ph > 8 && hist_.count >= 2) {
        // 找到这个引擎在 hist_.gpuEngine 里的槽位
        int slot = -1;
        switch (engineType) {
        case 1: slot = 0; break;   // 3D
        case 6: slot = 1; break;   // Copy
        case 3: slot = 2; break;   // Video Encode
        case 2: slot = 3; break;   // Video Decode
        default: break;
        }

        if (slot >= 0) {
            // 【多卡】选历史数组：实例有效时用该实例自己的引擎历史。
            const double* histArr = hist_.gpuEngine[slot];
            if (inst >= 0 && inst < PerfHistory::kMaxInstances)
                histArr = hist_.gpuEngineI[inst][slot];

            // 自适应量程：取历史最大值向上留 25%，下限 5%
            double mx = 0.0;
            for (int i = 0; i < hist_.count; ++i) {
                double v = hist_.At(histArr, i);
                if (v > mx) mx = v;
            }
            double ceilv = (mx > 0.5) ? (mx * 1.25) : 5.0;
            if (ceilv > 100.0) ceilv = 100.0;

            const int n = hist_.count;
            std::vector<POINT> pts;
            pts.reserve(n);
            for (int i = 0; i < n; ++i) {
                double v = hist_.At(histArr, i);
                double t = v / ceilv;
                if (t > 1) t = 1;
                if (t < 0) t = 0;
                int px = plot.left + (n > 1 ? i * (pw - 1) / (n - 1) : 0);
                int py = plot.bottom - static_cast<int>(t * (ph - 1));
                if (py < plot.top) py = plot.top;
                pts.push_back(POINT{ px, py });
            }

            // 填充
            std::vector<POINT> poly = pts;
            poly.push_back(POINT{ pts.back().x, plot.bottom });
            poly.push_back(POINT{ pts.front().x, plot.bottom });
            HBRUSH fb = CachedBrush(kChartFill);   // 【性能】GPU 引擎格 × 每帧
            HGDIOBJ ob = SelectObject(hdc, fb);
            HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
            Polygon(hdc, poly.data(), static_cast<int>(poly.size()));
            SelectObject(hdc, ob);
            SelectObject(hdc, op);

            // 曲线。用 kChartLineGpu 而不是 kChartLine ——
            // 两者当前数值相同（都是 #117DBB），但主图用的是 kChartLineGpu，
            // 引擎格用 kChartLine 会让「五页主色」那套语义在以后改色时分叉。
            HPEN pen = CachedPen(kChartLineGpu);
            HGDIOBJ oldp = SelectObject(hdc, pen);
            Polyline(hdc, pts.data(), static_cast<int>(pts.size()));
            SelectObject(hdc, oldp);
        }
    }

    // 外框：GPU 引擎格用 GPU 组的主色（#117DBB，与 CPU 同组）。
    // 【真原版截图实测】CPU 逻辑处理器格子的边框实测就是主色 #117DBB，不是浅网格色；
    // 旧实现用 kChartGrid（浅蓝）当边框，边框几乎看不见。这里改用主色。
    HGDIOBJ op = SelectObject(hdc, GetStockObject(DC_PEN));
    SetDCPenColor(hdc, kChartLineGpu);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);

    SelectObject(hdc, old);
}

void PerfPage::PaintContent(HDC hdc) const {
    RECT content{ bounds_.left + kSidebarWidth, bounds_.top,
                  bounds_.right, bounds_.bottom };

    if (!snap_) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
        SetBkMode(hdc, TRANSPARENT);
        RECT r = content;
        DrawText2(hdc, L"正在采集…", &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER, kSubText);
        SelectObject(hdc, old);
        return;
    }

    // 内容区内缩：
    // 【UIFILE 声明】performancetab_tables 根 padding = rect(17rp,0rp,0rp,0rp)
    //   （docs/evidence/exec/_S_geo2_30001.tsv @0x5180，S_duispec.md §1.3）→ 左内缩 17rp。
    // 【UIFILE 未定义】上下内缩 12、右内缩 17 无声明（UIFILE 该 padding 的上下右都是 0），
    //   保留旧值以免标题行与页签/工具条贴边。
    // ⚠ 必须与 HitTestChartToggle 的 inner 保持完全一致。
    RECT inner{ content.left + Rpx(kContentPadL), content.top + 12,
                content.right - Rpx(kContentPadL), content.bottom - 12 };

    switch (view_) {
    case PerfView::Cpu:       PaintCpuPage(hdc, inner); break;
    case PerfView::Memory:    PaintMemPage(hdc, inner); break;
    case PerfView::Disk:      PaintDiskPage(hdc, inner); break;
    case PerfView::Network:   PaintNetPage(hdc, inner); break;
    case PerfView::Gpu:       PaintGpuPage(hdc, inner); break;
    default: break;
    }
}

void PerfPage::Paint(HDC hdc) const {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&f);

    if (!fontBody_) const_cast<PerfPage*>(this)->BuildFonts();

    PaintSidebar(hdc);
    PaintContent(hdc);

    QueryPerformanceCounter(&t1);
    lastPaintMicros_ = static_cast<int>(
        (t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
}

// 【P0-7 边界】把当前视图的选中实例下标夹到 [0, count)。
// 侧栏项是按**上一轮**快照生成的，快照刷新后实例数可能变少（网卡掉线、
// 磁盘热插拔），所以绘制时每一页都要重新夹一次，不能让旧下标越界。
// 返回 -1 表示没有可用实例，调用方回退到「取第 0 个 / 不画」。
int PerfPage::ClampInstance(int count) const {
    if (count <= 0) return -1;
    const int vi = static_cast<int>(view_);
    int i = selInst_[vi];
    if (i < 0 || i >= count) i = 0;
    return i;
}

// ---- 侧栏滚动 ----

int PerfPage::SidebarViewportHeight() const {
    return bounds_.bottom - (bounds_.top + Rpx(kSidebarTopPad));
}

void PerfPage::SidebarLayout(std::vector<int>& tops, std::vector<int>& heights) const {
    tops.clear();
    heights.clear();
    tops.reserve(items_.size());
    heights.reserve(items_.size());
    int y = 0;                       // 相对内容顶部的偏移，绘制时再加起点
    for (size_t i = 0; i < items_.size(); ++i) {
        const int h = items_[i].detail.empty() ? kSidebarCardH2 : kSidebarCardH3;
        tops.push_back(y);
        heights.push_back(h);
        y += h;
    }
}

int PerfPage::SidebarContentHeight() const {
    std::vector<int> tops, heights;
    SidebarLayout(tops, heights);
    if (tops.empty()) return 0;
    return tops.back() + heights.back();
}

bool PerfPage::SidebarNeedsScroll() const {
    return SidebarContentHeight() > SidebarViewportHeight();
}

bool PerfPage::ScrollSidebar(int deltaPx) {
    const int content = SidebarContentHeight();
    const int view    = SidebarViewportHeight();
    const int maxScroll = content - view;
    if (maxScroll <= 0) {
        sidebarScroll_ = 0;
        return false;
    }
    int next = sidebarScroll_ + deltaPx;
    if (next < 0) next = 0;
    if (next > maxScroll) next = maxScroll;
    if (next == sidebarScroll_) return false;
    sidebarScroll_ = next;
    return true;
}

bool PerfPage::OnClick(int x, int y) {
    if (x < bounds_.left || x >= bounds_.left + kSidebarWidth) return false;
    if (y < bounds_.top + Rpx(kSidebarTopPad)) return false;
    // 同 OnHover：点击可能早于本帧绘制，必须保证 items_ 就位。
    EnsureSidebarItems();
    std::vector<int> tops, heights;
    SidebarLayout(tops, heights);
    const int top0 = bounds_.top + Rpx(kSidebarTopPad) - sidebarScroll_;
    for (size_t i = 0; i < items_.size(); ++i) {
        const int cardTop = top0 + tops[i];
        const int cardBot = cardTop + heights[i];
        if (y >= cardTop && y < cardBot) {
            selectedItem_ = static_cast<int>(i);
            // 【P0-7 核心】把卡片对应的**实例下标**记下来 —— 页面绘制读的是
            // 这个值，而不是侧栏行下标。以前只有 selectedItem_ 被写、被读，
            // items_[i].index 全项目零读取，于是「点磁盘 2 画磁盘 0」。
            // 按视图分槽存。CPU/内存卡在 RebuildSidebarItems 里 index 填的是 0，
            // 但那两个视图没有实例概念，ClampInstance 只被磁盘/网络/GPU 三页调用，
            // 所以这里的 0 不会被当成实例下标用。
            const int vi = static_cast<int>(items_[i].view);
            if (vi >= 0 && vi < static_cast<int>(PerfView::Count)) {
                selInst_[vi] = items_[i].index;
            }
            view_ = items_[i].view;
            itemsDirty_ = true;
            return true;
        }
    }
    return false;
}

void PerfPage::OnHover(int x, int y) {
    // 【必须在这里也保证 items_ 已建】OnHover 是 WM_MOUSEMOVE 进来的，
    // 可能早于本帧的 PaintSidebar。不保证的话 items_ 为空 → hover 恒不亮。
    EnsureSidebarItems();
    int idx = -1;
    if (x >= bounds_.left && x < bounds_.left + kSidebarWidth &&
        y >= bounds_.top + Rpx(kSidebarTopPad)) {
        std::vector<int> tops, heights;
        SidebarLayout(tops, heights);
        const int top0 = bounds_.top + Rpx(kSidebarTopPad) - sidebarScroll_;
        for (size_t i = 0; i < items_.size(); ++i) {
            const int cardTop = top0 + tops[i];
            if (y >= cardTop && y < cardTop + heights[i]) {
                idx = static_cast<int>(i);
                break;
            }
        }
    }
    hoverItem_ = idx;
}

} // namespace tmp
