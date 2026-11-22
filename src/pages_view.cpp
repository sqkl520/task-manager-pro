// pages_view.cpp - 四个次要页面的绘制实现
//
// ==========================================================================
// 列清单/列宽的权威来源（本轮据 taskmgr.exe 的 .rdata/.data 出厂默认表重建）
// ==========================================================================
// taskmgr.exe 里每页有一份「列池」：36 字节头 + count 条 56 字节记录。
// 头布局：+0 页 id / +4 记录条数 / +8 恒为 1 / +16 默认名称键指针（低 32 位）
// 记录布局（本轮实测确定）：
//     +0  列宽（0 = 自适应/弹性列）
//     +4  最小列宽（数值列 = 列宽，文本列 = 0）
//     +8  colId（列名 = STRINGTABLE 里 strId = colId + 1 的那条）
//     +12 该页内的槽位序号 0..n-1
//     +16 名称列标志（只有每页第 0 条是 255）
//     +20 内容类型位掩码
//     +28 / +36 / +40 / +44 内部角色序号
//     +36/+40 该列名称键（UTF-16 常量串）的 64 位指针
//   其余字段用途未证实
//
// 四页的表位置（文件偏移）与页 id：
//     pageId 0 进程        @0x129E90  15 条（记录从 +36 起）
//     pageId 2 应用历史记录 @0x12A880   8 条
//     pageId 3 启动        @0x12A1E0  10 条
//     pageId 4 用户        @0x12A530  11 条
//     （pageId 1 性能 / 5 详细信息 / 6 服务 不在 .data 里；
//       详细信息页用另外的「掩码+32+48 宽度+48 索引」记录 @0xF3ED0，掩码 0x209D；
//       服务页的列池在 .rdata @0xF4208，5 条：32046/32004/32038/32035/32047）
//
// 交叉验证（重要）：同一份页池在 HKCU 的 Preferences 二进制
// （UIFILE 30001 反编译，见 docs/evidence/uifile_Taskmgr_30001.xml）里逐字节重复了一次 —— 页 0 @0x50、
// 页 3 @0x3A0、页 4 @0x6F0、页 2 @0xA40，宽度/colId 与 exe 出厂值完全一致，
// 说明**用户从未改过这四页的列配置，屏上跑的就是 exe 出厂默认**。
//
// 注意：页池是「选择列」的全集，不是默认可见集合。
// 默认可见集合的位掩码只对详细信息页找到过（0x209D）；其余页未找到，
// 所以下面各表的**可见子集**按 MUI 文案 + 真机行为取，已逐条注明证据等级。
#include "pages_view.h"
#include "menus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <shellapi.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace tmp {

using namespace theme;

// ==========================================================================
// 本文件内使用的色值（theme.h 里没有、但有原版裁切/实测依据的）
// ==========================================================================

// 列头列间竖线。
// 【像素实测】x=184/234/643/743 处 1px 竖线，
// 色 rgb(244,244,244) / rgb(239,239,239)，只在列头带（逻辑 y≈5..24）出现，
// 数据区没有。取中值 #F2F2F2（比进程页用的 #D8D8D8 淡得多，
// 09_pages.md 5-3 明确指出「原版列头竖线非常淡，比进程页那条还淡」）。
constexpr COLORREF kLineHeaderCol = RGB(0xF2, 0xF2, 0xF2);

// 用户行左侧展开箭头（折叠态 ►）。
// 【像素实测（16x 放大）】墨迹 x=160..239 → 逻辑 10.0..14.9；
// 【实测 userpage_6x.png（6x）】x=90..119 → 逻辑 15.0..19.8。
// 两张不同倍数的裁切给出同样的 ~5px 宽箭头 → 交叉验证成立。
constexpr COLORREF kChevronColor = RGB(0x6D, 0x6D, 0x6D);

// 用户行头像占位块。
// 【实测】三张不同倍数裁切都是 **9×9 逻辑像素、纯色 #E6E6E6、无圆角**：
//   user_icon_16x  bbox 144×144 @16x  = 9.00×9.00 逻辑，x=32.0..40.9
//   user_row_10x   bbox  90×90  @10x  = 9.00×9.00 逻辑，x=37.0..45.9
//   userpage_6x    bbox  60×66  @6x   ≈ 10×11 逻辑（含 1px 抗锯齿，核心仍 9×9）
// 三个不同倍数一致 → 9×9 是硬结论。
constexpr COLORREF kUserAvatarBox = RGB(0xE6, 0xE6, 0xE6);

// 用户行缩进锚点（逻辑 x，相对表格左界）。
// chevron 10..15 / 头像块 32..41 / 用户名文字 51（user_icon_16x 的
// 「字母 A 墨迹 x=816..943 → 逻辑 51.0..58.9」其实就是 Administrator 的首字母，
// 即用户名文字的左沿）。
// 【单位】下面这几个锚点都是 96 DPI 基准值（原版裁切按 100% 缩放取的），
// 使用处必须经 Rpx() 换算，不能直接当像素加。
constexpr int kUserChevronX = 10;
constexpr int kUserAvatarX = 32;
constexpr int kUserTextX = 51;

// 服务行的 16×16 服务图标。
// 【像素实测】数据行逻辑 x=10..22 是图标
// （配色 rgb(165,201,240) 系浅蓝 + rgb(206,210,215) 灰，即蓝色齿轮），
// 逻辑 x=25 起是 AJRouter 的 A 字形。
// 【单位】同上，96 DPI 基准；使用处经 Rpx() 换算。
constexpr int kServiceIconX = 8;
constexpr int kServiceTextX = 25;
constexpr COLORREF kServiceGearFill = RGB(0xA5, 0xC9, 0xF0);
constexpr COLORREF kServiceGearLine = RGB(0xCE, 0xD2, 0xD7);

// ==========================================================================
// 数值格式
// ==========================================================================
//
// 【内部一致性】进程页（`tmwindow_list.cpp` 的 FmtCpuText / FmtMemText）
// 已经有一套格式，四个次要页必须完全一致，否则同一台机器上
// 「进程页 CPU 列显示 0%，用户页同一会话 CPU 列显示空白」这种自相矛盾。
// 那两个函数在 tmwindow_list.cpp 的匿名命名空间里，本文件拿不到，
// 因此在这里按同样的口径复刻（若将来上移到公共头，直接替换即可）。

namespace {

// CPU 百分比。原版 0 值明确显示 `0%`（原版实拍），不是空白。
void FmtCpu(double pct, wchar_t* buf, size_t cch) {
    if (pct < 0.05)      swprintf_s(buf, cch, L"0%%");
    else if (pct < 1.0)  swprintf_s(buf, cch, L"%.1f%%", pct);
    else                 swprintf_s(buf, cch, L"%.0f%%", pct);
}

// 内存。K / MB / GB 三档，0 值留空（与进程页一致）。
void FmtMem(uint64_t bytes, wchar_t* buf, size_t cch) {
    if (bytes == 0) { buf[0] = 0; return; }
    double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0)         swprintf_s(buf, cch, L"%.1f K", kb);
    else if (kb < 1048576.0) swprintf_s(buf, cch, L"%.1f MB", kb / 1024.0);
    else                     swprintf_s(buf, cch, L"%.1f GB", kb / 1048576.0);
}

// 服务行的 16rp×16rp 蓝色齿轮。
//
// 原版这个图标来自它的图像列表资源，本工程没有同款位图，这里用纯 GDI
// 复刻：外圈 8 个齿 + 浅蓝圆盘 + 灰描边 + 中心挖空。
// 16rp 下只求「一眼是那个蓝色齿轮」，不做细节。
//
// 【单位】下面所有偏移（中心 7、齿位 ±5/±7、齿块 −1..+2、圆盘 6/7、孔 3/4）
// 都是 96 DPI 基准值，逐个经 Rpx() 换算 —— 否则在 150% 下齿轮仍是 16px，
// 而它旁边的文字和行高都放大了，图标会明显偏小。
void DrawServiceGear(HDC hdc, int x, int y) {
    const int cx = x + Rpx(7);
    const int cy = y + Rpx(7);

    HBRUSH fillB = CreateSolidBrush(kServiceGearFill);

    // 8 个齿：每 45° 一个小方块，先画在圆盘外层
    for (int i = 0; i < 8; ++i) {
        static const int dx[8] = { 0,  5,  7,  5,  0, -5, -7, -5 };
        static const int dy[8] = { -7, -5, 0,  5,  7,  5,  0, -5 };
        RECT tooth{ cx + Rpx(dx[i]) - Rpx(1), cy + Rpx(dy[i]) - Rpx(1),
                    cx + Rpx(dx[i]) + Rpx(2), cy + Rpx(dy[i]) + Rpx(2) };
        FillRect(hdc, &tooth, fillB);
    }

    // 圆盘
    HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
    HGDIOBJ ob = SelectObject(hdc, fillB);
    Ellipse(hdc, cx - Rpx(6), cy - Rpx(6), cx + Rpx(7), cy + Rpx(7));
    SelectObject(hdc, ob);
    SelectObject(hdc, op);

    // 灰描边 + 中心孔。
    // 注意：中心孔**不能填白色** —— 选中行整行是蓝底（kBgNavSelected），
    // 白色圆点会在蓝底上留下一个刺眼的白洞。这里只画一圈灰环。
    // 笔宽保持 1 物理像素：这是线条不是几何，见本文件的 DPI 判断说明。
    HPEN pen = CreatePen(PS_SOLID, 1, kServiceGearLine);
    op = SelectObject(hdc, pen);
    ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, cx - Rpx(6), cy - Rpx(6), cx + Rpx(7), cy + Rpx(7));
    Ellipse(hdc, cx - Rpx(3), cy - Rpx(3), cx + Rpx(4), cy + Rpx(4));
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);

    DeleteObject(fillB);
}

// 用户行的展开箭头（折叠态 ►）。
// 尺寸取自 user_icon_16x（逻辑宽 4.94、居中于行高）与 userpage_6x（15.0..19.8）。
// 【单位】4 / 5 是 96 DPI 基准，经 Rpx() 换算。
void DrawRowChevron(HDC hdc, int x, int cy) {
    POINT tri[3];
    tri[0] = POINT{ x,           cy - Rpx(4) };
    tri[1] = POINT{ x + Rpx(5),  cy };
    tri[2] = POINT{ x,           cy + Rpx(4) };
    HBRUSH b = CreateSolidBrush(kChevronColor);
    HGDIOBJ ob = SelectObject(hdc, b);
    HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(b);
}

// 用户行的 9rp×9rp 纯色头像占位块（无圆角，见 kUserAvatarBox 的注释）。
// 【单位】4 / 9 是 96 DPI 基准，经 Rpx() 换算。
void DrawUserAvatarBox(HDC hdc, int x, int cy) {
    RECT box{ x, cy - Rpx(4), x + Rpx(9), cy + Rpx(5) };
    HBRUSH b = CreateSolidBrush(kUserAvatarBox);
    FillRect(hdc, &box, b);
    DeleteObject(b);
}

// 滚动偏移的 DPI 换算：把「旧 DPI 下的像素」折成「新 DPI 下的像素」。
//
// 【为什么必须折】scroll_ 是**像素**偏移（见 pages_view.h 的说明），不是行号。
// DPI 变了以后内容总高从 rows×Rpx(28) 按比例变大，而 scroll_ 若原样保留，
// 同一像素值在新布局里指向的是另一行 —— 用户会看到画面突然跳到约 2/3 的位置。
//
// 旧 DPI ≤ 0 时返回原值：那是 scroll_ 必然为 0 的初始状态（没必要折，也折不了）。
int RescaleScrollPx(int px, int fromDpi, int toDpi) {
    if (fromDpi <= 0 || toDpi <= 0 || fromDpi == toDpi) return px;
    return MulDiv(px, toDpi, fromDpi);
}

} // namespace

// ==========================================================================
// 列定义
// ==========================================================================

// ---------- 启动页 ----------
// 可见 4 列（默认集合）。
// 证据：STRINGTABLE 2314（36992 + slot）逐个读取得到启动页列族：
//   37002 名称 / 37006 发布者 / 37005 状态 / 37013 启动影响 /
//   37014 启动类型 / 37015 启动时的磁盘 I/O / 37016 启动时的 CPU /
//   37017 立即运行 / 37018 禁用时间 / 37008 命令行
// exe 页 3 列池（pageId 3，10 条）的宽度与顺序为：
//   名称 215 | 发布者 150 | 状态 100 | 启动影响 100 | 启动类型 100 |
//   启动时的磁盘 I/O 50 | 启动时的 CPU 50 | 立即运行 70 | 禁用时间 100 | 命令行 260
// 后 6 项是「选择列」里的可选项，默认不显示（本机 Preferences 里没有
// 启动页的非默认记录，且原版启动页只有 4 列栏位）。
// 列宽从 exe 出厂值取（此前 发布者 160 / 状态 90 是估的）。
const PagesView::ColumnDef PagesView::kStartupCols[] = {
    { L"名称",      0,   DT_LEFT,  true  },   // 弹性（exe 出厂宽 215 用作 flexW 的起始基准）
    { L"发布者",    150, DT_LEFT,  false },   // exe 页 3 槽位 1
    { L"状态",      100, DT_LEFT,  false },   // exe 页 3 槽位 2
    { L"启动影响",  100, DT_LEFT,  false },   // exe 页 3 槽位 3
};
const int PagesView::kStartupColCount = 4;

// ---------- 用户页 ----------
// 7 列，顺序与可见集合：
//   用户 | 状态 | CPU | 内存 | 磁盘 | 网络 | GPU
// 证据：exe 页 4 列池（pageId 4，11 条）：
//   用户 215 | ID 45 | 会话 100 | 客户端名称 100 | 状态 100 |
//   CPU 73 | 内存 73 | 磁盘 73 | 网络 73 | GPU 73 | GPU 引擎 100
// 「ID / 会话 / 客户端名称 / GPU 引擎」4 项属于「选择列」，默认不显示
// （原版用户页默认就 7 列）。
// 【此前缺 GPU 列】：因为「用户」列是唯一弹性列且带 220px 下限，
// 6 列已把 1084px 吃光，GPU 加上去也画不出来 —— 现在把 GPU 加进来，
// 并把用户列的下限改回 exe 出厂宽 215，7 列才能真正全部落到屏上。
const PagesView::ColumnDef PagesView::kUserCols[] = {
    { L"用户",  0,   DT_LEFT,  true },   // 弹性（exe 出厂宽 215 用作 flexW 的起始基准）
    { L"状态",  100, DT_LEFT,  false },  // exe 页 4 槽位 4（37005 状态）
    { L"CPU",   73,  DT_RIGHT, false },  // exe 页 4 槽位 5
    { L"内存",  73,  DT_RIGHT, false },  // exe 页 4 槽位 6
    { L"磁盘",  73,  DT_RIGHT, false },  // exe 页 4 槽位 7
    { L"网络",  73,  DT_RIGHT, false },  // exe 页 4 槽位 8
    { L"GPU",   73,  DT_RIGHT, false },  // exe 页 4 槽位 9（此前缺失）
};
const int PagesView::kUserColCount = 7;

// ---------- 服务页 ----------
// 5 列：名称 | PID | 描述 | 状态 | 组
// 证据一：exe .rdata @0xF4208 的 5 条列池
//   32046→名称 180 | 32004→PID 50 | 32038→描述 0 | 32035→状态 100 | 32047→组 100
//   注意「描述」的出厂宽度是 **0** —— 0 表示自适应，即服务页的弹性列是
//   **描述**，不是名称。此前把名称设成弹性列（吃掉 464px）是错的。
// 证据二：像素实测列边界逻辑 x=184/234/643/743，
//   即 名称≈184、PID 50、描述 409、状态 100、组 743..表格右沿。
// 校验：把 exe 出厂值代入「描述 = 内容宽 − 其余四列」：
//   184 + 50 + 409 + 100 = 743，再到表格右沿（约 845，滚动条轨道 845..861
//   见 `svc_header_4x.png` 右侧那条 rgb(240,240,240)）= 组宽 ≈102 ≈ 100。
//   → 出厂 180/50/0/100/100 与 4 倍裁切逐像素实测**完全自洽**，
//     组列最终取 100（不是 124 —— 124 是拿裁切图右边缘 867 算的，
//     而 867 已是含滚动条的画面右沿，不是表格右沿）。
const PagesView::ColumnDef PagesView::kServiceCols[] = {
    { L"名称",  180, DT_LEFT,  false },  // 固定
    { L"PID",   50,  DT_RIGHT, false },
    { L"描述",  0,   DT_LEFT,  true  },  // 弹性（exe 出厂宽 0 = 自适应）
    { L"状态",  100, DT_LEFT,  false },
    { L"组",    100, DT_LEFT,  false },
};
const int PagesView::kServiceColCount = 5;

// ---------- 应用历史记录页 ----------
// 5 列：名称 | CPU 时间 | 网络 | 磁贴更新 | 下载
// 证据：STRINGTABLE 2316（37040 起）：
//   37041 名称 / 37042 CPU 时间 / 37043 网络 / 37044 磁贴更新 /
//   37045 按流量计费的网络 / 37046 不按流量计费的网络 / 37047 下载 / 37048 上载
// exe 页 2 列池（pageId 2，8 条）：
//   名称 198 | CPU 时间 107 | 网络 107 | 按流量计费的网络 107 | 磁贴更新 107 |
//   不按流量计费的网络 160 | 下载 125 | 上载 125
// 【两处文案修正】
//   1) 第 4 列原文是「磁贴更新」（37044），此前写成「平铺更新」——同音错字，
//      MUI 全表搜「平铺」零结果。
//   2) 第 5 列原文是「下载」（37047），此前写成「应用内下载」。
//      MUI 全表搜「下载」只有三条：33459/33463/37047，没有「应用内下载」。
const PagesView::ColumnDef PagesView::kAppHistCols[] = {
    { L"名称",      0,   DT_LEFT,  true  },   // 弹性（exe 出厂宽 198 用作 flexW 的起始基准）
    { L"CPU 时间",  107, DT_RIGHT, false },
    { L"网络",      107, DT_RIGHT, false },
    { L"磁贴更新",  107, DT_RIGHT, false },   // 曾误写「平铺更新」
    { L"下载",      125, DT_RIGHT, false },   // 曾误写「应用内下载」
};
const int PagesView::kAppHistColCount = 5;

// ============ 基础 ============

PagesView::~PagesView() {
    for (auto& kv : iconCache_) {
        if (kv.second) DestroyIcon(kv.second);
    }
    auto del = [](HFONT& f) { if (f) { DeleteObject(f); f = nullptr; } };
    del(fontBody_); del(fontBold_); del(fontSmall_); del(fontTitle_);
}

void PagesView::BuildFonts() {
    fontsDpi_ = 0;      // 强制重建，见 EnsureFonts()
    EnsureFonts();
}

// 字体是按「建字体那一刻的 Dpi()」算死的：lfHeight = -Rpx(px)。窗口被拖到
// 另一块缩放不同的显示器上时 Dpi() 会变，而已建好的 HFONT 不会自己变大 ——
// 结果是行高、列宽都跟着新 DPI 放大，字却还是旧的 12px，行高 42 配 12px 字。
//
// 所以每个绘制入口先调这里：比一下建字体时的 DPI 和当前 Dpi()，不一致就重建。
// 字体族仍然沿用系统 message font（本机 = Microsoft YaHei UI），本轮只改字号；
// 【约束】lfHeight 传的是**字符高度**（负值 = 逻辑单位），必须经 Rpx() 换算，
// 不能把 12/10/14 直接当像素写死。
void PagesView::EnsureFonts() const {
    const int dpi = Dpi();
    if (fontsDpi_ == dpi && fontBody_ && fontBold_ && fontSmall_ && fontTitle_) return;

    auto del = [](HFONT& f) { if (f) { DeleteObject(f); f = nullptr; } };
    del(fontBody_); del(fontBold_); del(fontSmall_); del(fontTitle_);

    auto mk = [](int px96, int w) -> HFONT {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        LOGFONTW lf{};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            lf = ncm.lfMessageFont;
        } else {
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
        }
        // ncm 里的 lfHeight 是按系统 DPI 给的，这里整条覆写，不会二次缩放。
        lf.lfHeight = -Rpx(px96);
        lf.lfWeight = w;
        lf.lfQuality = CLEARTYPE_QUALITY;
        return CreateFontIndirectW(&lf);
    };
    fontBody_  = mk(kFontSizeBody, FW_NORMAL);
    // 列头字重 = 常规，与其他页保持一致。
    // 【实测】进程页列头「状态」的横笔宽度分布 {1:8, 2:18, 3:8, 4:6}
    // 与正文同量级，说明原版各页列头都是常规体。
    // 此前这里用 FW_SEMIBOLD，导致同一个视觉元素在进程页是常规、
    // 在启动/用户/服务/应用历史记录页是半粗 —— 跨页自相矛盾。
    fontBold_  = mk(kFontSizeHeader, FW_NORMAL);
    fontSmall_ = mk(10, FW_NORMAL);
    fontTitle_ = mk(14, FW_NORMAL);
    fontsDpi_ = dpi;
}

// 见 pages_view.h 的说明。把像素偏移按 DPI 比例折算。
//
// 【为什么不改 fontsDpi_】这个函数只负责 scroll_；字体由 EnsureFonts() 在
// 下一次绘制时自己发现并重建，两条路径互不依赖 —— 少调一次不至于让字永远错下去。
void PagesView::OnDpiChanged(int oldDpi) {
    const int newDpi = Dpi();
    if (oldDpi <= 0 || oldDpi == newDpi) return;

    scroll_ = RescaleScrollPx(scroll_, oldDpi, newDpi);

    // 折算后可能越界（行数没变时不会，但数据可能在这期间变了）。
    // 用 SetScrollPixels 而不是直接赋值，让钳制和 hover_ 清理只有一份实现。
    SetScrollPixels(scroll_);

    // 字体缓存键也要跟上：若调用方在 SetDpi 之后、下一次绘制之前调了这里，
    // 顺手把 fontsDpi_ 置 0 会让 EnsureFonts() 多重建一次 —— 无害但浪费。
    // 因此这里**不动** fontsDpi_，交给 EnsureFonts() 按 DPI 比对自行决定。
}

void PagesView::Fill(HDC hdc, const RECT& rc, COLORREF c) const {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &rc, b);
    DeleteObject(b);
}

void PagesView::HLine(HDC hdc, int x1, int x2, int y, COLORREF c) const {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ o = SelectObject(hdc, p);
    MoveToEx(hdc, x1, y, nullptr);
    LineTo(hdc, x2, y);
    SelectObject(hdc, o);
    DeleteObject(p);
}

// 1px 竖线。为了不扩大 pages_view.h 的私有接口（本次任务只允许改本文件），
// 这里用文件内的匿名命名空间函数实现，而不是再加一个成员函数。
namespace {
void DrawVLine(HDC hdc, int x, int y1, int y2, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ o = SelectObject(hdc, p);
    MoveToEx(hdc, x, y1, nullptr);
    LineTo(hdc, x, y2);
    SelectObject(hdc, o);
    DeleteObject(p);
}

// 排序折角的一格步进（像素）与总跨度。
//
// 【为什么不是 Rpx(9)】折角的 9 是一张 **9 行的像素图样**（每行一个 SetPixelV），
// 不是一段可以任意缩放的几何长度。步进取 Rpx(1) 的整数值，图样行数恒为 9，
// 这样高 DPI 下折角是「每格变大」而不是「行数变多」，同时保证相邻两行之间
// 不会出现空行（若把 9 行按浮点比例拉伸再逐个取整，150% 下会出现虚线）。
// 96 DPI 下 step == 1、跨度 == 9，与缩放前逐像素等价。
int ChevronStepPx() { const int s = Rpx(1); return s > 0 ? s : 1; }
int ChevronSpanPx() { return 9 * ChevronStepPx(); }
} // namespace

void PagesView::Text(HDC hdc, const wchar_t* s, RECT* rc, UINT fmt, COLORREF c) const {
    if (!s || !*s) return;
    SetTextColor(hdc, c);
    DrawTextW(hdc, s, -1, rc, fmt | DT_NOPREFIX);
}

// ============ 表格绘制 ============

// 排序指示器。与进程页列头同形（那边逐像素对齐过真原版）：
//   降序 ▼  升序 ▲
//   9rp 行高、核心 1rp 宽的细折角，抗锯齿由 GDI 的硬像素近似（无 alpha）。
// 这里不追求像素级 —— 次要页的列表与进程页不是同一套控件，
// 只保证「形状语义一致、颜色一致」。
//
// 【为什么用 SetPixelV 而不是 Polygon】折角本身是 1px 的像素图样，
// 每一格的步进必须整数像素；SetPixelV 是唯一能保证「一格一格」落点的画法。
// 步进经 Rpx() 后在高 DPI 下会变大，折角随之变大 —— 这是要的行为，
// 但折角的**行数仍是 9**（不是把 9 也乘一次），否则 150% 下会画成 13 行的高胖箭头。
std::wstring PagesView::ChevronPointsForTest(bool desc) {
    // 与 DrawSortChevron 完全同一组落点计算，用固定原点 (0,0) 与 step=1。
    //
    // 【必须排序】断言要判断的是「两次画出来的**像素集合**是否相同」，
    // 不是「绘制顺序是否相同」。旧图案左右对称，翻转行序后绘制顺序变了、
    // 点集没变 —— 不排序的话断言会假阳性通过（实测踩过：改回对称图案后
    // 断言仍 PASS，因为有序串不同但点集相同）。
    std::wstring s;
    wchar_t buf[32];
    std::vector<std::pair<int,int>> pts;
    for (int i = 0; i <= 4; ++i) {
        const int row = desc ? i : (8 - i);
        pts.push_back({i, row});
        pts.push_back({8 - i, row});
    }
    std::sort(pts.begin(), pts.end());
    for (const auto& p : pts) {
        swprintf_s(buf, L"%d,%d;", p.first, p.second);
        s += buf;
    }
    return s;
}

void PagesView::DrawSortChevron(HDC hdc, int cx, int cy, bool desc, COLORREF color) const {
    // 【图案必须能区分上下，旧图案不能】
    //
    // 旧 kCx = {0,1,2,3,3,3,2,1,0} 关于 i=4 对称。翻转只把行序倒过来
    // （row = desc ? i : 8-i），而 kCx[i] == kCx[8-i]，所以升序和降序
    // 落点逐像素相同 —— 实跑验证 10 个像素完全一致，方向根本看不出。
    //
    // 现在画一个真正的 V 形折角：宽度从两端向中心递增，但**上下不对称**——
    // 上半段窄、下半段宽。翻转后上半变宽、下半变窄，视觉上就是 ▲ / ▼。
    //
    // 每行落点：左臂 x = cx + i，右臂 x = cx + (8 - i)，i 从 0 到 4。
    // 行号 row 用 desc 决定方向，列位置与 row 无关 —— 这才是箭头。
    const int step = ChevronStepPx();
    for (int i = 0; i <= 4; ++i) {
        // desc = ▼：窄在上、宽在下；asc = ▲：宽在上、窄在下
        const int row = desc ? i : (8 - i);
        const int y = cy + row * step;
        // 左臂
        SetPixelV(hdc, cx + i * step, y, color);
        // 右臂（对称位置，间距随深度收窄）
        SetPixelV(hdc, cx + (8 - i) * step, y, color);
    }
}

void PagesView::PaintTableHeader(HDC hdc, const RECT& rc, const ColumnDef* cols,
                                 int nCols, int flexWidth) const {
    EnsureFonts();
    const int headerH = Rpx(kHeaderHeight);
    RECT hdr{ rc.left, rc.top, rc.right, rc.top + headerH };
    Fill(hdc, hdr, kBgHeader);
    HLine(hdc, hdr.left, hdr.right, hdr.bottom - 1, kLineHeader);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBold_));
    SetBkMode(hdc, TRANSPARENT);
    SetTextAlign(hdc, TA_LEFT | TA_TOP);

    int x = rc.left;
    for (int i = 0; i < nCols; ++i) {
        int w = cols[i].flexible ? flexWidth : Rpx(cols[i].width);

        // 列间竖线：只在列头带出现，数据区没有。
        // 【像素实测】x=184/234/643/743 处
        // 1px、rgb(244,244,244)~rgb(239,239,239)，纵向覆盖整个列头带，
        // 数据区 y=37.5 处这几列都是 rgb(246~251) 近白 —— 没有竖线。
        // 竖线本身的笔宽是 1 物理像素（不缩放，见 DPI 判断说明）；
        // 它离列头带上沿的内缩 4rp 要缩放。
        if (i > 0) {
            DrawVLine(hdc, x, hdr.top + Rpx(4), hdr.bottom - 1, kLineHeaderCol);
        }

        RECT cell{ x + Rpx(kCellPadLeft), hdr.top, x + w - Rpx(kCellPadRight), hdr.bottom };
        UINT fmt = DT_SINGLELINE | DT_VCENTER | cols[i].align;
        Text(hdc, cols[i].title, &cell, fmt, kTextHeader);

        // 【排序箭头】当前页的排序列画一个细折角（降序 ▼ / 升序 ▲）。
        // 位置：列名**右侧**。次要页的单元格左边距只有 4rp（kCellPadLeft），
        // 放在左边会压到列边界竖线上；放右边既不会撞列名，也与进程页的
        // 「箭头在文字旁」观感一致。
        if (CurSortCol() == i) {
            SIZE sz{};
            HGDIOBJ oldf = SelectObject(hdc, fontBold_);
            GetTextExtentPoint32W(hdc, cols[i].title, lstrlenW(cols[i].title), &sz);
            SelectObject(hdc, oldf);
            // sz.cx 已经包含了字体缩放（字体本身是 -Rpx(12)），**不要再乘一次**。
            // 只有「文字到箭头」的间隙 4 是几何常量，要缩放。
            const int cx = x + Rpx(kCellPadLeft) + sz.cx + Rpx(4);
            const int cy = hdr.top + (headerH - ChevronSpanPx()) / 2;
            DrawSortChevron(hdc, cx, cy, CurSortDesc(), kTextHeader);
        }

        x += w;
    }
    SelectObject(hdc, old);
}

// 【单位约定】本函数的 x / y / w / h 参数**一律是已换算好的像素**，
// 调用方传 flexW（已经由 bounds 算出来）、或 Rpx(cols[i].width)。
// 只有内边距 kCellPadLeft / kCellPadRight 在这里缩放 —— 它们是 96 DPI 常量。
void PagesView::PaintCellText(HDC hdc, const wchar_t* text, int x, int y, int w, int h,
                              UINT align, COLORREF color) const {
    RECT cell{ x + Rpx(kCellPadLeft), y, x + w - Rpx(kCellPadRight), y + h };
    UINT fmt = DT_SINGLELINE | DT_VCENTER | align | DT_END_ELLIPSIS;
    Text(hdc, text, &cell, fmt, color);
}

void PagesView::FillRow(HDC hdc, int x, int y, int w, int h, int rowIndex,
                        bool selected, bool hovered) const {
    RECT row{ x, y, x + w, y + h };
    if (selected) {
        Fill(hdc, row, kBgNavSelected);
    } else if (hovered) {
        // 必须用 theme 常量。这里曾硬编码 RGB(0xE5,0xF3,0xFB)，
        // 蓝通道比 theme::kRowHover(#E5F3FF) 少 4 级 —— 同一台机器上
        // 进程页和次要页的悬停色不同，肉眼能看出偏灰。
        Fill(hdc, row, kRowHover);
    }
    // 无交替条纹（与进程页一致）
    (void)rowIndex;
}

int PagesView::HitRow(int y, int rowCount) const {
    const int listTop = bounds_.top + Rpx(kHeaderHeight);
    if (y < listTop) return -1;
    const int rh = Rpx(kRowHeight);
    int idx = (y - listTop + scroll_) / rh;
    if (idx < 0 || idx >= rowCount) return -1;
    return idx;
}

HICON PagesView::GetIconFor(const std::wstring& path) const {
    if (path.empty()) return nullptr;
    auto it = iconCache_.find(path);
    if (it != iconCache_.end()) return it->second;

    // 【无界增长修复】见 include/pages_view.h 的 kIconCacheMax 注释。
    // 超上限时整表清空（DestroyIcon 释放位图）再重建，避免长跑累积。
    if (iconCache_.size() >= kIconCacheMax) {
        for (auto& kv : iconCache_) {
            if (kv.second) DestroyIcon(kv.second);
        }
        iconCache_.clear();
    }

    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON)) {
        iconCache_[path] = sfi.hIcon;
        return sfi.hIcon;
    }
    iconCache_[path] = nullptr;
    return nullptr;
}

// ============ 刷新数据 ============

void PagesView::RefreshStartup() {
    StartupSource src;
    if (!startup_.empty()) {
        // 已有数据只更新状态，避免重复解析发布者
        std::vector<StartupItem> fresh;
        src.Enumerate(fresh);
        startup_ = std::move(fresh);
    } else {
        src.Enumerate(startup_);
    }
}

void PagesView::RefreshUsers() {
    UserSource src;
    src.Enumerate(users_);

    // 按 sessionId 聚合进程快照
    if (snap_) {
        for (auto& u : users_) {
            u.cpuPercent = 0.0;
            u.workingSet = 0;
            u.processCount = 0;
        }
        for (const auto& p : snap_->processes) {
            // 【排除 PID 0】System Idle Process 的 CPU 时间等于系统空闲时间。
            // 不排除的话会被算成 session 0 的占用 —— 实测 71.56%，真实值 2.66%，
            // 虚高 68.9 个百分点。进程页、详细信息页、历史曲线都已排除它
            //（tmwindow.cpp:564/652/714/739、tmwindow_details.cpp:483），
            // 只有这里漏了。
            if (p.pid == 0) continue;
            for (auto& u : users_) {
                if (static_cast<uint32_t>(p.sessionId) == u.sessionId) {
                    u.cpuPercent += p.cpuPercent;
                    // 【口径统一】私有工作集优先，与进程页内存列一致。
                    // 原来累加的是 workingSet（总工作集），共享 DLL 页会在
                    // 同一会话的多个进程间被重复计入，导致这一页的「内存」
                    // 比把进程页该会话各行相加高出约 40%（实测 1.40 倍）。
                    u.workingSet += p.privateBytes ? p.privateBytes : p.workingSet;
                    ++u.processCount;
                    break;
                }
            }
        }
    }
}

void PagesView::RefreshServices() {
    // 重排前记住当前选中的服务名（selService_ 是索引，排序后会指到别的服务）
    std::wstring keepName;
    if (selService_ >= 0 && selService_ < static_cast<int>(services_.size())) {
        keepName = services_[selService_].name;
    }

    ServiceSource src;
    src.Enumerate(services_);
    // 排序口径：按**服务短名**升序。
    //
    // 【理由】原版服务页名称列显示的是服务短名（原版实拍
    // 的 `asusm` 那一行是决定性证据），排序跟随显示列是 ListView 的默认行为；
    // 而 `ServiceSource::Enumerate` 内部按 `displayName` 排（
    // `pages_data_users_services.cpp:143`），与屏上看到的那一列不是同一个键，
    // 于是行序会与「名称列看起来是升序」矛盾。
    // 这里在数据装进视图后按 name 重排一次；不改数据层，只改本视图的顺序。
    std::sort(services_.begin(), services_.end(),
              [](const ServiceItem& a, const ServiceItem& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });

    // 恢复选中
    if (!keepName.empty()) {
        for (size_t i = 0; i < services_.size(); ++i) {
            if (_wcsicmp(services_[i].name.c_str(), keepName.c_str()) == 0) {
                selService_ = static_cast<int>(i);
                break;
            }
        }
    }
    enrichCursor_ = 0;

    // 【重启服务的后半程】RestartServiceAsync 只做了「停」，把服务名记在
    // pendingRestart_。这里在下一轮刷新时发现它确实停了，再启动它。
    // 这样「重启」全程不阻塞 UI 线程——原来是用 Sleep(600) 同步等待，
    // 点一下整个窗口 0.6 秒不响应消息，标题栏显示「(未响应)」。
    if (!pendingRestart_.empty()) {
        for (size_t i = 0; i < services_.size(); ++i) {
            if (_wcsicmp(services_[i].name.c_str(), pendingRestart_.c_str()) != 0) continue;
            // canStart 为真 = 已经停了（SCM 说可以启动）。还在跑就再等一轮：
            // 停止是异步的，通常 1~3 秒才到位。
            if (!services_[i].canStart) break;
            StartService(static_cast<int>(i));
            pendingRestart_.clear();
            break;
        }
    }
}

void PagesView::RestartServiceAsync(int index) {
    if (index < 0 || index >= static_cast<int>(services_.size())) return;
    // 先记下名字（停止后行序可能变，索引不可信），再异步停
    pendingRestart_ = services_[index].name;
    StopService(index);
}

bool PagesView::EnrichServicesBatch(int maxCount) {
    if (enrichCursor_ >= static_cast<int>(services_.size())) return false;

    ServiceSource src;
    int done = 0;
    while (enrichCursor_ < static_cast<int>(services_.size()) && done < maxCount) {
        src.EnrichOne(services_[enrichCursor_]);
        ++enrichCursor_;
        ++done;
    }
    return enrichCursor_ < static_cast<int>(services_.size());
}

void PagesView::RefreshAppHistory() {
    AppHistorySource src;
    src.Enumerate(appHistory_);
}

// ============ 启动页 ============

void PagesView::PaintStartup(HDC hdc) const {
    EnsureFonts();
    RECT rc = bounds_;
    const int headerH = Rpx(kHeaderHeight);
    const int rowH = Rpx(kRowHeight);

    int fixed = 0;
    for (int i = 0; i < kStartupColCount; ++i) {
        if (!kStartupCols[i].flexible) fixed += Rpx(kStartupCols[i].width);
    }
    // 弹性列最小宽度 = 名称列的 exe 出厂宽 215（96 DPI 基准，要缩放）。
    // 注意：不能用「max(下限, 剩余)」这种写法——窗口比下限还窄时会溢出到
    // 表格右界之外，把行底色画到滚动条/窗口边框上。这里改成剩余宽度不足
    // 下限时才用下限，并且所有单元格仍按同一组 x 推进，保证列对齐。
    const int minFlexW = Rpx(215);
    int avail = static_cast<int>(rc.right - rc.left) - fixed;
    int flexW = (avail > minFlexW) ? avail : minFlexW;

    PaintTableHeader(hdc, rc, kStartupCols, kStartupColCount, flexW);

    // 空状态。
    // 【MUI 硬证据】STRINGTABLE 里 39001 = 「没有可显示的启动项」，全表唯一一条。
    // 此前 startup_ 为空时列表区纯白，什么都不画。
    if (startup_.empty()) {
        RECT msg{ rc.left, rc.top + headerH, rc.right, rc.bottom };
        HFONT oldF = static_cast<HFONT>(SelectObject(hdc, fontBody_));
        SetBkMode(hdc, TRANSPARENT);
        Text(hdc, L"没有可显示的启动项", &msg,
             DT_SINGLELINE | DT_VCENTER | DT_CENTER, kTextSecondary);
        SelectObject(hdc, oldF);
        return;
    }

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    // 【裁剪】行区之外的像素一律不画 —— 与进程页 PaintRows 同一类缺陷。
    //
    // `scroll_` 上一轮从「行号」改成了「像素」（ScrollBy: `scroll_ += lines * kRowHeight`），
    // 同时 `maxScroll = contentH - viewH` 也改成像素口径。麻烦在于
    // `viewH = bounds_.bottom - bounds_.top - kHeaderHeight` 一般**不是**
    // kRowHeight 的整数倍，所以滚到底（或任何被钳到 maxScroll 的位置）时
    // `scroll_` 带一个非整行的余数，第一行的 y 就小于 `rc.top + kHeaderHeight`。
    //
    // 下面的守卫 `if (y + kRowHeight <= rc.top + kHeaderHeight) continue;`
    // 只跳过**整行都在表头之上**的情况，半行重叠时照样往下画；
    // 而 FillRow / DrawIconEx / Text 都是直接落 DC 的，行底色会把表头盖住。
    //
    // 【IntersectClipRect 的边界值不缩放】它是 DC 裁剪矩形，参数是像素坐标；
    // 这里的 top 用 headerH（已缩放），左右用 rc 的原值 —— 没有任何 ±1 修正量。
    const int savedDC = SaveDC(hdc);
    IntersectClipRect(hdc, rc.left, rc.top + headerH, rc.right, rc.bottom);

    int y = rc.top + headerH - scroll_;
    const int listBottom = rc.bottom;

    for (size_t i = 0; i < startup_.size(); ++i) {
        if (y + rowH <= rc.top + headerH) { y += rowH; continue; }
        if (y >= listBottom) break;

        const StartupItem& it = startup_[i];
        FillRow(hdc, rc.left, y, rc.right - rc.left, rowH,
                static_cast<int>(i), static_cast<int>(i) == selStartup_,
                static_cast<int>(i) == hover_);

        COLORREF tc = (static_cast<int>(i) == selStartup_) ? kTextSelected : kTextPrimary;

        int x = rc.left;

        // 名称（带 16rp 图标）
        // 图标尺寸用 Rpx(kIconSize)（theme.h：16rp = SM_CXSMICON @96），
        // 不是写死 16 —— 高 DPI 下系统图标资源本身也是按 DPI 取的。
        const int iconSz = Rpx(kIconSize);
        if (!it.exePath.empty()) {
            HICON ico = GetIconFor(it.exePath);
            if (ico) {
                DrawIconEx(hdc, x + Rpx(8), y + (rowH - iconSz) / 2, ico, iconSz, iconSz,
                           0, nullptr, DI_NORMAL);
            }
        }
        PaintCellText(hdc, it.name.c_str(), x + iconSz + Rpx(12), y,
                      flexW - iconSz - Rpx(12), rowH, DT_LEFT, tc);
        x += flexW;

        PaintCellText(hdc, it.publisher.c_str(), x, y, Rpx(kStartupCols[1].width), rowH,
                      DT_LEFT, tc);
        x += Rpx(kStartupCols[1].width);

        PaintCellText(hdc, it.enabled ? L"已启用" : L"已禁用", x, y,
                      Rpx(kStartupCols[2].width), rowH, DT_LEFT, tc);
        x += Rpx(kStartupCols[2].width);

        // 启动影响。
        //
        // 【取值域来自 MUI，共 6 档 + 1 个特例】
        //   36012..36016「非常低 / 低 / 中 / 高 / 非常高」（性能页块）
        //   37499..37503「无 / 未计量 / 低 / 中 / 高」（启动项页块，紧跟
        //                 「访问被拒绝。」和右键菜单「启用(&N)/禁用(&A)」）
        //   40011「不适用」
        // 配套说明文字：
        //   39009「尚未计量影响。请重启电脑以计量启动影响。」
        //   39006「CPU 和磁盘活动的影响程度(在启动期间计量且每次重启时都会更新)」
        // 语义：没有 WDI StartupInfo 计量数据（首次开机 / 本机未跑过 WPT）
        //       → 正确取值是「未计量」，不是「不适用」。
        //       「不适用」只给不参与计量的项（如「文件复制例外」）。
        // 本工程没有计量数据来源，所以这里恒为「未计量」。
        // 【此前】无条件写死「不适用」并用 kTextSecondary 次级灰 ——
        //   既选错了档（原版无数据时是「未计量」），也和同行其他列不同色。
        PaintCellText(hdc, L"未计量", x, y, Rpx(kStartupCols[3].width), rowH,
                      DT_LEFT, tc);

        y += rowH;
    }

    SelectObject(hdc, old);
    RestoreDC(hdc, savedDC);   // 与函数开头的 SaveDC / IntersectClipRect 配对
}

bool PagesView::OnClickStartup(int x, int y) {
    (void)x;
    int idx = HitRow(y, static_cast<int>(startup_.size()));
    if (idx >= 0) {
        selStartup_ = idx;
        return true;
    }
    return false;
}

bool PagesView::ToggleStartup(int index) {
    if (index < 0 || index >= static_cast<int>(startup_.size())) return false;
    StartupSource src;
    bool want = !startup_[index].enabled;
    if (src.SetEnabled(startup_[index], want)) {
        startup_[index].enabled = want;
        if (!want) GetSystemTimeAsFileTime(&startup_[index].disabledTime);
        return true;
    }
    return false;
}

// ============ 用户页 ============

void PagesView::PaintUsers(HDC hdc) const {
    EnsureFonts();
    RECT rc = bounds_;
    const int headerH = Rpx(kHeaderHeight);
    const int rowH = Rpx(kRowHeight);
    // 各列宽在这里算一次（96 DPI 常量 → 像素）。绘制与命中测试都经同一组
    // Rpx() 换算，所以两边不会漂移。
    int colW[kUserColCount];
    int fixed = 0;
    for (int i = 0; i < kUserColCount; ++i) {
        colW[i] = kUserCols[i].flexible ? 0 : Rpx(kUserCols[i].width);
        if (!kUserCols[i].flexible) fixed += colW[i];
    }
    const int minFlexW = Rpx(215);
    int avail = static_cast<int>(rc.right - rc.left) - fixed;
    int flexW = (avail > minFlexW) ? avail : minFlexW;

    PaintTableHeader(hdc, rc, kUserCols, kUserColCount, flexW);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    // 【裁剪】同 PaintStartup：`scroll_` 是像素且 maxScroll 一般不是行高整数倍，
    // 本页的行循环**连整行跳过都没有**（只有 `if (y >= rc.bottom) break;`），
    // 所以第一行只要带一点向上偏移就会直接把表头盖掉。
    const int savedDC = SaveDC(hdc);
    IntersectClipRect(hdc, rc.left, rc.top + headerH, rc.right, rc.bottom);

    int y = rc.top + headerH - scroll_;

    for (size_t i = 0; i < users_.size(); ++i) {
        if (y >= rc.bottom) break;

        const UserSession& u = users_[i];
        FillRow(hdc, rc.left, y, rc.right - rc.left, rowH,
                static_cast<int>(i), static_cast<int>(i) == selUser_,
                static_cast<int>(i) == hover_);

        COLORREF tc = (static_cast<int>(i) == selUser_) ? kTextSelected : kTextPrimary;
        int x = rc.left;

        // 用户名单元格 = 展开箭头 + 9rp 头像占位块 + 「域\用户」文字，
        // 三个元素的横向锚点全部来自原版裁切（见文件头常量注释）：
        //   chevron 10..15 / 头像块 32..41 / 文字 51
        // 【此前】这里只有一行从 x+8 起画的纯文字，整组视觉元素全缺，
        // 用户名比原版左移 43px。
        const int rowCy = y + rowH / 2;
        DrawRowChevron(hdc, x + Rpx(kUserChevronX), rowCy);
        DrawUserAvatarBox(hdc, x + Rpx(kUserAvatarX), rowCy);

        std::wstring display;
        // 【显示完整账户名(&S)】选项菜单里这一项只在**用户页**可点（menus.cpp:197），
        // 但它唯一的消费点以前在 tmwindow_details.cpp 的用户名列 —— 也就是
        // 「在用户页勾上、在详细信息页才生效」，而且勾选态随切页消失。
        // 现在直接在用户页消费：关 = 「域\用户」，开 = 仅用户名（原版语义是
        // 「完整账户名」即含域；这里按开关名反向给两种形态，保证两项都可见效果）。
        // ⚠ 无原版像素证据说明两种形态的确切文本，标注为行为对齐而非像素对齐。
        const bool fullName = MenuBuilder::ShowFullNameOption();
        if (!u.domain.empty() && !u.userName.empty()) {
            display = fullName ? (u.domain + L"\\" + u.userName) : u.userName;
        } else if (!u.userName.empty()) {
            display = u.userName;
        } else {
            // session 0 等系统会话没有用户
            wchar_t b[64];
            swprintf_s(b, L"会话 %u", u.sessionId);
            display = b;
        }

        // 直接按绝对 x 画，不走 PaintCellText 的 kCellPadLeft，
        // 否则文字会跑到 51+4=55。
        {
            RECT cell{ x + Rpx(kUserTextX), y, x + flexW - Rpx(kCellPadRight), y + rowH };
            Text(hdc, display.c_str(), &cell,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS, tc);
        }
        x += flexW;

        PaintCellText(hdc, u.stateText.c_str(), x, y, colW[1], rowH, DT_LEFT, tc);
        x += colW[1];

        // 数值列格式与进程页完全一致（FmtCpu：0 值显示 0%，<1% 保留一位小数）
        wchar_t buf[64];
        FmtCpu(u.cpuPercent, buf, 64);
        PaintCellText(hdc, buf, x, y, colW[2], rowH, DT_RIGHT, tc);
        x += colW[2];

        FmtMem(u.workingSet, buf, 64);
        PaintCellText(hdc, buf, x, y, colW[3], rowH, DT_RIGHT, tc);
        x += colW[3];

        // 磁盘 / 网络 / GPU：没有 per-session 计数器，留空（与原版一致）
        PaintCellText(hdc, L"", x, y, colW[4], rowH, DT_RIGHT, tc);
        x += colW[4];
        PaintCellText(hdc, L"", x, y, colW[5], rowH, DT_RIGHT, tc);
        x += colW[5];
        PaintCellText(hdc, L"", x, y, colW[6], rowH, DT_RIGHT, tc);

        y += rowH;
    }

    SelectObject(hdc, old);
    RestoreDC(hdc, savedDC);   // 与函数开头的 SaveDC / IntersectClipRect 配对
}

bool PagesView::OnClickUsers(int x, int y) {
    (void)x;
    int idx = HitRow(y, static_cast<int>(users_.size()));
    if (idx >= 0) {
        selUser_ = idx;
        return true;
    }
    return false;
}

// ============ 服务页 ============

void PagesView::PaintServices(HDC hdc) const {
    EnsureFonts();
    RECT rc = bounds_;
    const int headerH = Rpx(kHeaderHeight);
    const int rowH = Rpx(kRowHeight);

    // 【已删除底部 40px 自绘按钮栏】
    // 原版服务页底部没有任何按钮栏：原版实拍
    // （逻辑 867×54）实测只有两组文字墨迹 ——
    //   x=0..104   「简略信息(D)」（圆圈图标 + 文字）
    //   x=121..191 「打开服务」（蓝色放大镜 + 链接色文字）
    // 整张图背景纯白 #FFFFFF（3364/3372 像素），**没有 1px 灰边框矩形按钮**，
    // 也搜不到「服务(S)」这个字符串（MUI 全表零结果）。
    // 那条工具条由 tmwindow_toolbar.cpp 的 PaintToolbar 负责，不在这里画。
    // 保留按钮栏还会让表格少 40px、最后一行被硬裁。
    RECT tableRc = rc;

    int colW[kServiceColCount];
    int fixed = 0;
    for (int i = 0; i < kServiceColCount; ++i) {
        colW[i] = kServiceCols[i].flexible ? 0 : Rpx(kServiceCols[i].width);
        if (!kServiceCols[i].flexible) fixed += colW[i];
    }
    const int minFlexW = Rpx(120);
    int avail = static_cast<int>(tableRc.right - tableRc.left) - fixed;
    int flexW = (avail > minFlexW) ? avail : minFlexW;

    PaintTableHeader(hdc, tableRc, kServiceCols, kServiceColCount, flexW);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    // 【裁剪】同 PaintStartup：滚动量是像素，半行重叠的守卫挡不住行底色。
    const int savedDC = SaveDC(hdc);
    IntersectClipRect(hdc, tableRc.left, tableRc.top + headerH,
                      tableRc.right, tableRc.bottom);

    int y = tableRc.top + headerH - scroll_;

    for (size_t i = 0; i < services_.size(); ++i) {
        if (y + rowH <= tableRc.top + headerH) { y += rowH; continue; }
        if (y >= tableRc.bottom) break;

        const ServiceItem& s = services_[i];
        FillRow(hdc, tableRc.left, y, tableRc.right - tableRc.left, rowH,
                static_cast<int>(i), static_cast<int>(i) == selService_,
                static_cast<int>(i) == hover_);

        COLORREF tc = (static_cast<int>(i) == selService_) ? kTextSelected : kTextPrimary;
        int x = tableRc.left;

        // 名称列：**服务短名**（不是显示名）。
        //
        // 【硬证据：原版 4 倍裁切（未随仓库分发）】
        //   名称列 = `AJRouter`（服务短名）、描述列 = `AllJoyn Router Service`（显示名）
        // 【第二行独立复核：同一张 4 倍裁切】三行里
        //   `asusm` / `ASUS 更新服务 (asasm)` / `AsusUpdateCheck`
        //   —— 名称列显示 `asusm`，若用显示名这里应为「ASUS 更新服务 (asasm)」。
        //   这一行是决定性证据。
        // 此前名称列用 displayName、描述列用 SERVICE_CONFIG_DESCRIPTION 的长描述段落，
        // **两列内容全错位**（306 行 × 2 列）。
        // 齿轮的 y 偏移用 Rpx(kIconSize) 而不是写死 16 —— 高 DPI 下齿轮本身
        // 也按 Rpx 放大（见 DrawServiceGear），两者必须同源，否则图标偏上/偏下。
        DrawServiceGear(hdc, x + Rpx(kServiceIconX), y + (rowH - Rpx(kIconSize)) / 2);
        {
            // 文字左沿 = 逻辑 25（原版实测），不走 PaintCellText 的 4rp 内边距
            RECT cell{ x + Rpx(kServiceTextX), y, x + colW[0] - Rpx(kCellPadRight),
                       y + rowH };
            Text(hdc, s.name.c_str(), &cell,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS, tc);
        }
        x += colW[0];

        wchar_t buf[32];
        buf[0] = 0;
        if (s.pid != 0) swprintf_s(buf, L"%u", s.pid);
        PaintCellText(hdc, buf, x, y, colW[1], rowH, DT_RIGHT, tc);
        x += colW[1];

        // 描述列 = **显示名**（DisplayName）。原版这一列不是长描述文本，
        // 长描述只在「属性」对话框/工具提示里出现。
        // 颜色用正文色 —— `svc_header_4x.png` 里 `AllJoyn Router Service`
        // 与名称列 `AJRouter` 是同一种近黑，不是次级灰。
        PaintCellText(hdc, s.displayName.c_str(), x, y, flexW, rowH, DT_LEFT, tc);
        x += flexW;

        PaintCellText(hdc, s.stateText.c_str(), x, y, colW[3], rowH, DT_LEFT, tc);
        x += colW[3];

        PaintCellText(hdc, s.group.c_str(), x, y, colW[4], rowH, DT_LEFT, tc);

        y += rowH;
    }

    SelectObject(hdc, old);
    RestoreDC(hdc, savedDC);   // 与函数开头的 SaveDC / IntersectClipRect 配对
}

bool PagesView::OnClickServices(int x, int y) {
    (void)x;
    // 【已删除底部按钮栏热区】
    // 原实现在 y >= bounds_.bottom - 40 时无差别 ShellExecuteW("services.msc")，
    // 即底部 40px 那一条**从左到右任何像素**（包括按钮之间的空白）都会
    // 弹出 services.msc；而表格正好被这条遮住，用户想点最后一行服务就会
    // 意外弹窗。原版底部工具条只有「打开服务」那一小块热区，
    // 由 tmwindow_toolbar.cpp 的 PaintToolbar/命中测试负责。
    int idx = HitRow(y, static_cast<int>(services_.size()));
    if (idx >= 0) {
        selService_ = idx;
        return true;
    }
    return false;
}

bool PagesView::StartService(int index) {
    if (index < 0 || index >= static_cast<int>(services_.size())) return false;
    ServiceSource src;
    return src.Start(services_[index].name);
}

bool PagesView::StopService(int index) {
    if (index < 0 || index >= static_cast<int>(services_.size())) return false;
    ServiceSource src;
    return src.Stop(services_[index].name);
}

// ============ 应用历史记录页 ============

void PagesView::PaintAppHistory(HDC hdc) const {
    EnsureFonts();
    RECT rc = bounds_;
    const int headerH = Rpx(kHeaderHeight);

    // 顶部说明条高 44rp。它以及下面所有偏移都是 96 DPI 基准值。
    const int noteH = Rpx(44);

    // 顶部说明条（MUI 原文 38509：自 %1!s! 以来，当前用户和系统账户的资源使用情况。）
    RECT note{ rc.left, rc.top, rc.right, rc.top + noteH };
    Fill(hdc, note, RGB(0xFA, 0xFA, 0xFA));
    HLine(hdc, note.left, note.right, note.bottom - 1, kLineNavBorder);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    // 说明文字框：左内缩 12、上内缩 6、高 20、右留 140（给按钮让位）。
    RECT nr{ note.left + Rpx(12), note.top + Rpx(6),
             note.right - Rpx(140), note.top + Rpx(26) };
    Text(hdc, L"自 30 天前以来，当前用户和系统账户的资源使用情况。", &nr,
         DT_SINGLELINE | DT_VCENTER | DT_LEFT, kTextSecondary);

    // "删除使用情况历史记录" 按钮（MUI 38505）
    RECT btn{ note.right - Rpx(172), note.top + Rpx(8),
              note.right - Rpx(12), note.top + Rpx(32) };
    Fill(hdc, btn, RGB(0xE1, 0xE1, 0xE1));
    HGDIOBJ op = SelectObject(hdc, GetStockObject(DC_PEN));
    SetDCPenColor(hdc, RGB(0xAD, 0xAD, 0xAD));
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, btn.left, btn.top, btn.right, btn.bottom);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    RECT btr = btn;
    Text(hdc, L"删除使用情况历史记录", &btr,
         DT_SINGLELINE | DT_VCENTER | DT_CENTER, kTextPrimary);

    // 表格
    RECT tableRc{ rc.left, note.bottom, rc.right, rc.bottom };
    int fixed = 0;
    for (int i = 0; i < kAppHistColCount; ++i) {
        if (!kAppHistCols[i].flexible) fixed += Rpx(kAppHistCols[i].width);
    }
    const int minFlexW = Rpx(198);
    int avail = static_cast<int>(tableRc.right - tableRc.left) - fixed;
    int flexW = (avail > minFlexW) ? avail : minFlexW;
    PaintTableHeader(hdc, tableRc, kAppHistCols, kAppHistColCount, flexW);

    // SRUM 不可用 → 显示 "无可用数据"（MUI 38507 原文，原版在 SRUM 被禁用时也这么显示）
    if (appHistory_.empty()) {
        // 位置与原版无从比对（原版裁切图里只有
        // 「应用历史记录」六个字的放大，不含页面内容），这里在表格区内居中，
        // 与启动页空状态用同一种居中口径，保持四页内部一致。
        RECT msg{ tableRc.left, tableRc.top + headerH, tableRc.right, tableRc.bottom };
        // 与启动页空状态同色同位，四页口径一致（此前是 kTextDisabled 更浅的灰）
        Text(hdc, L"无可用数据", &msg,
             DT_SINGLELINE | DT_VCENTER | DT_CENTER, kTextSecondary);
    }

    SelectObject(hdc, old);
}

// ============ 悬停与滚动 ============
//
// 这两个函数此前完全不存在，导致 hover_ 与 scroll_ 从初始化后再没人写过：
// 行悬停永远不出现，服务页 300 多项只有前十几行能看。事件层一旦调用它们，
// 两处绘制分支（PaintUsers/PaintServices/PaintAppHistory 里的 `i == hover_`
// 与 `kHeaderHeight - scroll_`）就会立刻活过来。

// 当前页的列定义与弹性宽度基准。四个页各有一套（见 kXxxCols 的注释）。
// 返回 nullptr 表示当前页没有表格（性能页等）。
//
// 【flexW 的单位是像素】返回前已经取过 Rpx()。调用方（HitHeaderAt）拿到的
// 直接是像素宽度；而 cols[i].width 仍是 96 DPI 基准值，调用方要自己 Rpx()。
// 这两者单位不同是刻意的：flexW 依赖运行时 bounds_，cols[].width 是常量表。
const PagesView::ColumnDef* PagesView::CurCols(int& nCols, int& flexW) const {
    const RECT& rc = bounds_;
    const int width = static_cast<int>(rc.right - rc.left);
    int fixed = 0;
    switch (currentTab_) {
    case 3:   // 启动
        for (int i = 0; i < kStartupColCount; ++i)
            if (!kStartupCols[i].flexible) fixed += Rpx(kStartupCols[i].width);
        nCols = kStartupColCount;
        flexW = (width - fixed > Rpx(215)) ? width - fixed : Rpx(215);
        return kStartupCols;
    case 2:   // 应用历史
        for (int i = 0; i < kAppHistColCount; ++i)
            if (!kAppHistCols[i].flexible) fixed += Rpx(kAppHistCols[i].width);
        nCols = kAppHistColCount;
        flexW = (width - fixed > Rpx(198)) ? width - fixed : Rpx(198);
        return kAppHistCols;
    case 4:   // 用户
        for (int i = 0; i < kUserColCount; ++i)
            if (!kUserCols[i].flexible) fixed += Rpx(kUserCols[i].width);
        nCols = kUserColCount;
        flexW = (width - fixed > Rpx(215)) ? width - fixed : Rpx(215);
        return kUserCols;
    case 6:   // 服务
        for (int i = 0; i < kServiceColCount; ++i)
            if (!kServiceCols[i].flexible) fixed += Rpx(kServiceCols[i].width);
        nCols = kServiceColCount;
        flexW = (width - fixed > Rpx(120)) ? width - fixed : Rpx(120);
        return kServiceCols;
    default:
        nCols = 0;
        flexW = 0;
        return nullptr;
    }
}

int PagesView::HitHeaderAt(int x, int y) const {
    // 只在表头带内
    if (y < bounds_.top || y >= bounds_.top + Rpx(kHeaderHeight)) return -1;
    if (x < bounds_.left || x >= bounds_.right) return -1;

    int nCols = 0, flexW = 0;
    const ColumnDef* cols = CurCols(nCols, flexW);
    if (!cols) return -1;

    int cx = bounds_.left;
    for (int i = 0; i < nCols; ++i) {
        const int w = cols[i].flexible ? flexW : Rpx(cols[i].width);
        if (x >= cx && x < cx + w) return i;
        cx += w;
    }
    return -1;
}

bool PagesView::ClickHeader(int col) {
    if (currentTab_ < 0 || currentTab_ >= kMaxSortPages) return false;
    int nCols = 0, flexW = 0;
    const ColumnDef* cols = CurCols(nCols, flexW);
    if (!cols || col < 0 || col >= nCols) return false;

    if (sortCol_[currentTab_] == col) {
        // 同列再点：翻转方向
        sortDesc_[currentTab_] = !sortDesc_[currentTab_];
    } else {
        // 换列：从升序开始
        sortCol_[currentTab_] = col;
        sortDesc_[currentTab_] = false;
    }
    ApplySort();
    scroll_ = 0;        // 重排后回到顶部，否则会停在不相干的位置
    hover_ = -1;
    return true;
}

// 按当前页的排序状态重排数据。没有排序（sortCol < 0）时保持数据源顺序。
void PagesView::ApplySort() {
    const int col = CurSortCol();
    const bool desc = CurSortDesc();
    if (col < 0) return;

    auto cmpStr = [desc](const std::wstring& a, const std::wstring& b) {
        const int r = _wcsicmp(a.c_str(), b.c_str());
        return desc ? (r > 0) : (r < 0);
    };

    switch (currentTab_) {
    case 3:   // 启动：名称 / 发布者 / 状态 / 启动影响
        if (col < 4) {
            std::sort(startup_.begin(), startup_.end(),
                [&](const StartupItem& a, const StartupItem& b) {
                    switch (col) {
                    case 0: return cmpStr(a.name, b.name);
                    case 1: return cmpStr(a.publisher, b.publisher);
                    case 2: {
                        // 启用 > 禁用（desc 时反过来）
                        const bool ea = a.enabled, eb = b.enabled;
                        if (ea != eb) return desc ? (ea < eb) : (ea > eb);
                        return false;
                    }
                    default:
                        // 「启动影响」目前整列都是「未计量」——StartupItem 里
                        // 没有影响等级字段（要从 StartupApproved 或历史记录推，
                        // 成本高且不稳定）。该列点排序不改变顺序，直接 false。
                        (void)a; (void)b;
                        return false;
                    }
                });
        }
        break;

    case 6:   // 服务：名称 / PID / 描述 / 状态 / 组
        if (col < 5) {
            std::sort(services_.begin(), services_.end(),
                [&](const ServiceItem& a, const ServiceItem& b) {
                    switch (col) {
                    case 0: return cmpStr(a.name, b.name);
                    case 1: {
                        if (a.pid != b.pid) return desc ? (a.pid > b.pid) : (a.pid < b.pid);
                        return false;
                    }
                    case 2: return cmpStr(a.displayName, b.displayName);
                    case 3: return cmpStr(a.stateText, b.stateText);
                    default: return cmpStr(a.group, b.group);
                    }
                });
        }
        break;

    case 4:   // 用户：用户 / 状态 / CPU / 内存 /(磁盘 网络 GPU 无数据源)
        // UserSession 只聚合了 cpuPercent 与 workingSet 两项；磁盘/网络/GPU
        // 三列目前没有数据源（见 pages_data.h 的字段列表），对它们排序无意义。
        if (col < 4) {
            std::sort(users_.begin(), users_.end(),
                [&](const UserSession& a, const UserSession& b) {
                    switch (col) {
                    case 0: return cmpStr(a.userName, b.userName);
                    case 1: return cmpStr(a.stateText, b.stateText);
                    case 2: return desc ? (a.cpuPercent > b.cpuPercent) : (a.cpuPercent < b.cpuPercent);
                    default: return desc ? (a.workingSet > b.workingSet) : (a.workingSet < b.workingSet);
                    }
                });
        }
        break;

    default:
        break;
    }
}

int PagesView::HitRowAt(int x, int y) const {
    if (x < bounds_.left || x >= bounds_.right) return -1;
    switch (currentTab_) {
        case 3: return HitRow(y, static_cast<int>(startup_.size()));
        case 4: return HitRow(y, static_cast<int>(users_.size()));
        case 6: return HitRow(y, static_cast<int>(services_.size()));
        case 2: return HitRow(y, static_cast<int>(appHistory_.size()));
        default: return -1;
    }
}

void PagesView::OnHover(int x, int y) {
    if (currentTab_ < 0) { hover_ = -1; return; }

    // 移出窗口 / 落在列头或表格外 → 取消悬停
    if (x < 0 || y < 0 || !PtInRect(&bounds_, POINT{x, y})) {
        hover_ = -1;
        return;
    }
    const int listTop = bounds_.top + Rpx(kHeaderHeight);
    if (y < listTop) { hover_ = -1; return; }

    // 每个页的行数不同，取当前页的那一个
    int rowCount = 0;
    switch (currentTab_) {
        case 3: rowCount = static_cast<int>(startup_.size());    break;  // TabId::Startup
        case 4: rowCount = static_cast<int>(users_.size());      break;  // TabId::Users
        case 6: rowCount = static_cast<int>(services_.size());   break;  // TabId::Services
        case 2: rowCount = static_cast<int>(appHistory_.size()); break;  // TabId::AppHistory
        default: hover_ = -1; return;
    }
    const int idx = (y - listTop + scroll_) / Rpx(kRowHeight);
    hover_ = (idx >= 0 && idx < rowCount) ? idx : -1;
}

// 当前页的数据行数。四个次要页各有一份独立数据源，这里只做分发。
// 无表格的页（性能页 / 进程页 / 详细信息页）返回 0。
int PagesView::CurRowCount() const {
    switch (currentTab_) {
        case 3: return static_cast<int>(startup_.size());    // TabId::Startup
        case 4: return static_cast<int>(users_.size());      // TabId::Users
        case 6: return static_cast<int>(services_.size());   // TabId::Services
        case 2: return static_cast<int>(appHistory_.size()); // TabId::AppHistory
        default: return 0;
    }
}

// 数据区高度（像素）= 页边界高 - 列头高。
int PagesView::CurViewHeight() const {
    const int h = static_cast<int>(bounds_.bottom - bounds_.top) - Rpx(kHeaderHeight);
    return h > 0 ? h : 0;
}

// 可滚到的最大像素偏移。
//
// 【与绘制同源】四页的绘制都是 `y = rc.top + kHeaderHeight - scroll_`，
// 行高统一 kRowHeight，所以内容总高就是 rowCount × kRowHeight。
// 这里算出来的 maxScroll 与 ScrollBy / ScrollByPixels 的钳制、以及
// 绘制侧"最后一行能完整露出"三者用的是同一个式子。
//
// 【DPI】四处必须**同一个 Rpx() 口径**。若绘制侧用 Rpx(kRowHeight) 而这里用
// 96 DPI 的裸 28，150% 下内容高会被算成实际的三分之二 —— 滚到底时最后
// 一大截行永远到不了，且不会报错。下面 CurRowCount() * Rpx(kRowHeight)
// 与 PaintXxx 的 rowH 是同一个表达式。
int PagesView::MaxScrollPixels() const {
    if (currentTab_ < 0) return 0;
    const int contentH = CurRowCount() * Rpx(kRowHeight);
    const int viewH = CurViewHeight();
    return (contentH > viewH) ? (contentH - viewH) : 0;
}

// 精确像素位移。滚轮 / 触控板 / 触屏 / 惯性定时器全部走这里 ——
// 它是 scroll_ 的**唯一写入口**之外的第二道闸（SetScrollPixels 是第三道）。
void PagesView::ScrollByPixels(int px) {
    if (px == 0) return;
    SetScrollPixels(scroll_ + px);
}

// 直接设定滚动偏移（钳制到 [0, maxScroll]）。
// 【为什么要有不按行取整的入口】惯性滑行的速度是 px/帧 的浮点数，
// 一帧可能只有 1~2px；若每一次都换算成整数行，这些位移会被全部吃掉，
// 表现为"甩一把没反应、只滚一格"。
void PagesView::SetScrollPixels(int px) {
    const int maxScroll = MaxScrollPixels();
    int v = px;
    if (v < 0) v = 0;
    if (v > maxScroll) v = maxScroll;
    if (v == scroll_) return;
    scroll_ = v;

    // 滚动后原悬停行已经不在原位，直接清掉，等下一次 WM_MOUSEMOVE 重算
    hover_ = -1;
}

void PagesView::ScrollBy(int lines) {
    if (currentTab_ < 0 || lines == 0) return;

    // 【像素单位】scroll_ 是像素。滚轮传进来的是"行数"，这里乘行高。
    //
    // 原来 scroll_ 是行号、绘制却直接减（当像素用），滚一整行只移动 1 像素；
    // 而 maxScroll 是行数，永远无法停在两行之间。两头都不对。
    //
    // 现在：内容和视口都用像素算，maxScroll 可以不是 kRowHeight 的整数倍，
    // 滚到底时最后一行能完整露出，不会像原来那样留 22px 空白。
    // 钳制逻辑统一在 SetScrollPixels 里，这里不再重复一份（两处各写一遍必漂移）。
    ScrollByPixels(lines * Rpx(kRowHeight));
}

// ==========================================================================
// DPI 自测
// ==========================================================================
//
// 【为什么必须有它】本机 96 DPI，Rpx() 恒等于恒等函数 —— 所有改动在本机
// 逐像素等价，肉眼看不出任何差别，"看起来对"在这里没有任何信息量。
// 唯一能判真假的办法是把 Dpi() 强行设成 120/144/192 再回读数值。
//
// 断言的都是**可判定为假**的性质：具体数值、比例、单调整性。
// 不做"打印一下看看"这种无判据的输出。
std::wstring PagesView::SelfTestDpi() {
    wchar_t buf[512];

    // 退出时恢复原 DPI。用 RAII 而不是在每个 return 前手写一遍 ——
    // 中途 return 忘了恢复会让整个进程后续的所有 Scaled() 都按 192 算。
    const int savedDpi = theme::Dpi();
    struct DpiGuard {
        int d;
        ~DpiGuard() { theme::SetDpi(d); }
    } guard{ savedDpi };

    // ---- 1) Rpx(28) 在四个典型 DPI 下的绝对值 ----
    //
    // 28 是 theme.h 的 kRowHeight（UIFILE 里写死的 28rp），也是四个次要页
    // 唯一共用的行高，选它做基准最有代表性。
    struct Case { int dpi; int want28; int want180; };
    const Case cases[] = {
        {  96, 28, 180 },
        { 120, 35, 225 },
        { 144, 42, 270 },
        { 192, 56, 360 },
    };
    for (const Case& c : cases) {
        theme::SetDpi(c.dpi);
        const int got28 = Rpx(28);
        if (got28 != c.want28) {
            swprintf_s(buf, L"Rpx(28) 在 %d DPI 下应为 %d，实得 %d",
                       c.dpi, c.want28, got28);
            return buf;
        }
        // 顺带把服务页名称列宽 180 也逐个 DPI 钉死。
        const int got180 = Rpx(180);
        if (got180 != c.want180) {
            swprintf_s(buf, L"Rpx(180) 在 %d DPI 下应为 %d，实得 %d",
                       c.dpi, c.want180, got180);
            return buf;
        }
    }

    // ---- 2) 具体列宽在 96 与 144 下的比例应约等于 1.5 ----
    theme::SetDpi(96);
    const int w96 = Rpx(180);
    theme::SetDpi(144);
    const int w144 = Rpx(180);
    if (w96 <= 0) {
        wchar_t b[128];
        swprintf_s(b, L"96 DPI 下列宽为 %d，无法计算比例", w96);
        return b;
    }
    // 用整数交叉相乘判 1.5：w144 * 2 == w96 * 3。精确，不受浮点误差影响。
    if (w144 * 2 != w96 * 3) {
        swprintf_s(buf, L"列宽 180rp 在 144/96 下比例应为 1.5（%d→%d），交叉相乘不等",
                   w96, w144);
        return buf;
    }

    // ---- 3) 其余几何常量必须跟着同一个比例走 ----
    //
    // 这一项防的是「改了列宽忘了行高 / 忘了内边距」这种漏改：
    // 逐个比对 144 下的值是不是 96 下值的 1.5 倍。
    const int scaledConsts[] = { kRowHeight, kHeaderHeight, kIconSize,
                                 kCellPadLeft, kCellPadRight,
                                 kUserChevronX, kUserAvatarX, kUserTextX,
                                 kServiceIconX, kServiceTextX };
    theme::SetDpi(96);
    int v96[sizeof(scaledConsts) / sizeof(scaledConsts[0])];
    for (size_t i = 0; i < sizeof(scaledConsts) / sizeof(scaledConsts[0]); ++i)
        v96[i] = Rpx(scaledConsts[i]);
    theme::SetDpi(144);
    for (size_t i = 0; i < sizeof(scaledConsts) / sizeof(scaledConsts[0]); ++i) {
        const int want = v96[i] * 3 / 2;
        const int got = Rpx(scaledConsts[i]);
        // 允许 1px 的整数取整差（例如 25rp 在 144 下是 37.5 → 38 或 37）。
        if (got < want - 1 || got > want + 1) {
            swprintf_s(buf, L"几何常量 %drp 在 96/144 下应为 %d→~%d，实得 %d",
                       scaledConsts[i], v96[i], want, got);
            return buf;
        }
    }

    // ---- 4) 列宽之和必须等于表格宽度（布局自洽，任何 DPI 下都成立）----
    //
    // 这一项验证的不只是缩放，而是「绘制与命中测试用的是同一组换算」：
    // CurCols() 给出的 flexW 加各固定列宽，必须正好铺满 bounds_ 的宽度。
    // 若不成立，最后一列会和滚动条重叠或多出一截白条 —— 96 DPI 下也会错，
    // 所以它在任何机器上都是有效断言。
    {
        PagesView pv;
        const int dpis[] = { 96, 144 };
        for (int d : dpis) {
            theme::SetDpi(d);
            const int tableW = 900;   // 随便一个宽度，只需自洽
            pv.SetBounds(RECT{ 0, 0, tableW, 600 });
            pv.SetCurrentTab(6);      // 服务页：4 固定 + 1 弹性
            int nCols = 0, flexW = 0;
            const ColumnDef* cols = pv.CurCols(nCols, flexW);
            if (!cols || nCols != kServiceColCount) {
                swprintf_s(buf, L"%d DPI 下服务页列数为 %d，应为 %d",
                           d, nCols, kServiceColCount);
                return buf;
            }
            int sum = 0;
            for (int i = 0; i < nCols; ++i)
                sum += cols[i].flexible ? flexW : Rpx(cols[i].width);
            if (sum != tableW) {
                swprintf_s(buf, L"%d DPI 下服务页列宽之和 %d ≠ 表格宽 %d",
                           d, sum, tableW);
                return buf;
            }
        }
    }

    // ---- 5) 字体：lfHeight 必须走 Rpx()，且 DPI 变了要重建 ----
    //
    // 【怎么判定】拿 TEXTMETRIC 的 tmHeight 做证据。字体若仍是 96 DPI 的
    // -12，144 下 tmHeight 还是 17；走了 Rpx(12)=18 才会变大。
    // 同时验证 EnsureFonts() 的缓存键：故意只改 Dpi() 不调 BuildFonts()，
    // 下一次绘制前 EnsureFonts() 必须自己发现并重建。
    {
        PagesView pv;

        theme::SetDpi(96);
        pv.EnsureFonts();
        const int dpiAfter96 = pv.fontsDpi_;
        if (dpiAfter96 != 96) {
            swprintf_s(buf, L"EnsureFonts() 记录的 DPI 为 %d，应为 96", dpiAfter96);
            return buf;
        }
        auto measureBody = [&pv]() -> int {
            HDC dc = GetDC(nullptr);
            if (!dc) return -1;
            HGDIOBJ old = SelectObject(dc, pv.fontBody_);
            TEXTMETRICW tm{};
            const int h = GetTextMetricsW(dc, &tm) ? static_cast<int>(tm.tmHeight) : -1;
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
            return h;
        };
        const int tm96 = measureBody();
        if (tm96 <= 0) return L"96 DPI 下取不到 fontBody_ 的 TEXTMETRIC";

        // 只改 DPI，**不**调 BuildFonts —— 模拟 WM_DPICHANGED 的真实时序。
        theme::SetDpi(144);
        pv.EnsureFonts();          // 应该自己发现 fontsDpi_ != 144 并重建
        if (pv.fontsDpi_ != 144) {
            swprintf_s(buf, L"DPI 改为 144 后 EnsureFonts() 未重建（fontsDpi_=%d）",
                       pv.fontsDpi_);
            return buf;
        }
        const int tm144 = measureBody();
        if (tm144 <= tm96) {
            swprintf_s(buf, L"144 DPI 的字高 %d 不大于 96 DPI 的 %d —— lfHeight 没走 Rpx()",
                       tm144, tm96);
            return buf;
        }

        // ---- 6) 文字宽度：已受字体影响，不得二次缩放 ----
        //
        // PaintTableHeader 里 `cx = x + Rpx(4) + sz.cx` 直接用 sz.cx。
        // 若有人把它写成 Rpx(sz.cx)，144 下箭头会偏到列外。
        // 判据：144 下的文字宽度应约等于 96 下的 1.5 倍（字体本身已经缩放过），
        // 允许字距 hinting 造成的偏差，但不能接近 2.25 倍（= 二次缩放）。
        auto measureText = [&pv]() -> int {
            HDC dc = GetDC(nullptr);
            if (!dc) return -1;
            HGDIOBJ old = SelectObject(dc, pv.fontBold_);
            SIZE sz{};
            const int rc = GetTextExtentPoint32W(dc, L"启动影响", 4, &sz) ? sz.cx : -1;
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
            return rc;
        };
        theme::SetDpi(96);
        pv.EnsureFonts();
        const int tx96 = measureText();
        theme::SetDpi(144);
        pv.EnsureFonts();
        const int tx144 = measureText();
        if (tx96 <= 0 || tx144 <= 0) return L"取不到列头文字宽度";
        // 1.5 倍附近：tx144×2 应落在 tx96×3 的 ±25% 内。
        // 上限一侧卡在 1.85 倍，低于「二次缩放」会得到的 ~2.25 倍。
        if (tx144 * 2 < tx96 * 3 * 3 / 4 || tx144 * 2 > tx96 * 3 * 37 / 20) {
            swprintf_s(buf, L"文字宽度 96→144 为 %d→%d，比例异常（疑似二次缩放或未缩放）",
                       tx96, tx144);
            return buf;
        }
    }

    // ---- 7) 真渲染回读：列头底线所在的 y ----
    //
    // 【为什么只回读常量不够】前面几项验的都是 Rpx() 本身和常量表 ——
    // 它们只能证明"算术对"，不能证明 PaintServices 真的调了 Rpx()。
    // 漏改一个绘制点，前面六项照样全绿。
    //
    // 这里把 PaintServices 真画进一张离屏 DIB，再扫描列头底线的位置：
    //   PaintTableHeader 用 HLine(..., hdr.bottom - 1, kLineHeader) 画 #A0A0A0，
    //   hdr.bottom = bounds_.top + Rpx(kHeaderHeight)。
    // 96 DPI → 直线在第 23 行；144 DPI → Rpx(24)=36，直线在第 35 行。
    // 只要有一条不成立，就是绘制点没走 DPI 换算。
    {
        const int dpis[] = { 96, 144 };
        const int wantLineY[] = { 23, 35 };   // Rpx(24) - 1
        for (int k = 0; k < 2; ++k) {
            theme::SetDpi(dpis[k]);
            PagesView pv;
            pv.EnsureFonts();
            const int W = 400, H = 300;
            pv.SetBounds(RECT{ 0, 0, W, H });
            pv.SetCurrentTab(6);          // 服务页（不需要数据，表头总会画）

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;   // 负 = 自上而下，y 与屏幕一致
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC screen = GetDC(nullptr);
            HDC dc = screen ? CreateCompatibleDC(screen) : nullptr;
            HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
            if (!dc || !bmp || !bits) {
                if (bmp) DeleteObject(bmp);
                if (dc) DeleteDC(dc);
                if (screen) ReleaseDC(nullptr, screen);
                return L"创建离屏 DIB 失败，无法做渲染回读";
            }
            HGDIOBJ oldBmp = SelectObject(dc, bmp);
            // 先刷白，否则未绘制区域是 DIB 的初值 0（黑），也会命中扫描条件
            RECT all{ 0, 0, W, H };
            HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(dc, &all, wb);
            DeleteObject(wb);

            pv.PaintServices(dc);
            GdiFlush();

            // 在 x=5（名称列内，不与任何竖线重合）自上而下找 #A0A0A0。
            const COLORREF want = kLineHeader;
            const COLORREF* px = static_cast<const COLORREF*>(bits);
            int foundY = -1;
            for (int y = 0; y < H; ++y) {
                if (px[y * W + 5] == want) { foundY = y; break; }
            }

            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            if (screen) ReleaseDC(nullptr, screen);

            if (foundY != wantLineY[k]) {
                swprintf_s(buf,
                    L"%d DPI 下渲染回读的列头底线 y=%d，应为 %d"
                    L"（PaintTableHeader 没走 Rpx 或 HLine 位置不对）",
                    dpis[k], foundY, wantLineY[k]);
                return buf;
            }
        }
    }

    // ---- 9) 滚动偏移的 DPI 折算 ----
    //
    // scroll_ 是像素不是行号。DPI 从 96 变到 144 后，同一像素指向的行变了，
    // 必须按比例折算才能"停在同一个位置"。这里直接验 RescaleScrollPx 的算术
    // 和一个真实对象上的行为（OnDpiChanged 之后偏移按 1.5 倍变大且仍被钳制）。
    {
        struct SCase { int px, from, to, want; };
        const SCase sc[] = {
            { 0,   96, 144, 0   },
            { 100, 96, 144, 150 },
            { 150, 144, 96, 100 },
            { 300, 96, 192, 600 },
            { 100, 96,  96, 100 },   // 同 DPI 不变
        };
        for (const SCase& c : sc) {
            const int got = RescaleScrollPx(c.px, c.from, c.to);
            if (got != c.want) {
                swprintf_s(buf, L"滚动偏移 %dpx 从 %d 折到 %d DPI 应为 %d，实得 %d",
                           c.px, c.from, c.to, c.want, got);
                return buf;
            }
        }

        // 真实对象上只验「不越界、可重复调用、不破坏状态」。
        //
        // 【为什么不验"500px → 750px"】
        // 那需要 pages_ 里真的有行。注入行数的唯一办法是加一个只给自测用的
        // setter，而那正是本项目反复出现的「假功能」形态（有接口、无真实消费
        // 路径）。用真实数据源（SCM/WTS）又会让自测依赖机器状态。
        // 所以折算的**算术**用上面的纯函数用例钉死（5 个用例，覆盖 0 值、
        // 正反向、跨 2 倍、同 DPI），对象层只验它不会把状态改坏。
        {
            PagesView pv;
            pv.SetBounds(RECT{ 0, 0, 400, 200 });
            pv.SetCurrentTab(3);           // 启动页（本对象没有数据源，行数 0）
            theme::SetDpi(96);
            const int before = pv.ScrollPixels();
            theme::SetDpi(144);
            pv.OnDpiChanged(96);
            pv.OnDpiChanged(96);           // 重复调用必须幂等
            if (pv.ScrollPixels() != before) {
                swprintf_s(buf, L"无数据时 OnDpiChanged 把滚动偏移从 %d 改成了 %d",
                           before, pv.ScrollPixels());
                return buf;
            }
            if (pv.ScrollPixels() < 0 || pv.ScrollPixels() > pv.MaxScrollPixels()) {
                swprintf_s(buf, L"OnDpiChanged 之后滚动偏移 %d 越界（maxScroll=%d）",
                           pv.ScrollPixels(), pv.MaxScrollPixels());
                return buf;
            }
        }
    }

    return std::wstring();   // 空 = 全过
}

} // namespace tmp
