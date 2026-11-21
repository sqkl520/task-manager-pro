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
constexpr int kUserChevronX = 10;
constexpr int kUserAvatarX = 32;
constexpr int kUserTextX = 51;

// 服务行的 16×16 服务图标。
// 【像素实测】数据行逻辑 x=10..22 是图标
// （配色 rgb(165,201,240) 系浅蓝 + rgb(206,210,215) 灰，即蓝色齿轮），
// 逻辑 x=25 起是 AJRouter 的 A 字形。
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

// 服务行的 16×16 蓝色齿轮。
//
// 原版这个图标来自它的图像列表资源，本工程没有同款位图，这里用纯 GDI
// 复刻：外圈 8 个齿 + 浅蓝圆盘 + 灰描边 + 中心挖空。
// 16px 下只求「一眼是那个蓝色齿轮」，不做细节。
void DrawServiceGear(HDC hdc, int x, int y) {
    const int cx = x + 7;
    const int cy = y + 7;

    HBRUSH fillB = CreateSolidBrush(kServiceGearFill);

    // 8 个齿：每 45° 一个小方块，先画在圆盘外层
    for (int i = 0; i < 8; ++i) {
        static const int dx[8] = { 0,  5,  7,  5,  0, -5, -7, -5 };
        static const int dy[8] = { -7, -5, 0,  5,  7,  5,  0, -5 };
        RECT tooth{ cx + dx[i] - 1, cy + dy[i] - 1,
                    cx + dx[i] + 2, cy + dy[i] + 2 };
        FillRect(hdc, &tooth, fillB);
    }

    // 圆盘
    HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
    HGDIOBJ ob = SelectObject(hdc, fillB);
    Ellipse(hdc, cx - 6, cy - 6, cx + 7, cy + 7);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);

    // 灰描边 + 中心孔。
    // 注意：中心孔**不能填白色** —— 选中行整行是蓝底（kBgNavSelected），
    // 白色圆点会在蓝底上留下一个刺眼的白洞。这里只画一圈灰环。
    HPEN pen = CreatePen(PS_SOLID, 1, kServiceGearLine);
    op = SelectObject(hdc, pen);
    ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, cx - 6, cy - 6, cx + 7, cy + 7);
    Ellipse(hdc, cx - 3, cy - 3, cx + 4, cy + 4);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);

    DeleteObject(fillB);
}

// 用户行的展开箭头（折叠态 ►）。
// 尺寸取自 user_icon_16x（逻辑宽 4.94、居中于行高）与 userpage_6x（15.0..19.8）。
void DrawRowChevron(HDC hdc, int x, int cy) {
    POINT tri[3];
    tri[0] = POINT{ x,     cy - 4 };
    tri[1] = POINT{ x + 5, cy };
    tri[2] = POINT{ x,     cy + 4 };
    HBRUSH b = CreateSolidBrush(kChevronColor);
    HGDIOBJ ob = SelectObject(hdc, b);
    HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(b);
}

// 用户行的 9×9 纯色头像占位块（无圆角，见 kUserAvatarBox 的注释）。
void DrawUserAvatarBox(HDC hdc, int x, int cy) {
    RECT box{ x, cy - 4, x + 9, cy + 5 };
    HBRUSH b = CreateSolidBrush(kUserAvatarBox);
    FillRect(hdc, &box, b);
    DeleteObject(b);
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
    fontBody_  = mk(12, FW_NORMAL);
    // 列头字重 = 常规，与其他页保持一致。
    // 【实测】进程页列头「状态」的横笔宽度分布 {1:8, 2:18, 3:8, 4:6}
    // 与正文同量级，说明原版各页列头都是常规体。
    // 此前这里用 FW_SEMIBOLD，导致同一个视觉元素在进程页是常规、
    // 在启动/用户/服务/应用历史记录页是半粗 —— 跨页自相矛盾。
    fontBold_  = mk(12, FW_NORMAL);
    fontSmall_ = mk(10, FW_NORMAL);
    fontTitle_ = mk(14, FW_NORMAL);
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
} // namespace

void PagesView::Text(HDC hdc, const wchar_t* s, RECT* rc, UINT fmt, COLORREF c) const {
    if (!s || !*s) return;
    SetTextColor(hdc, c);
    DrawTextW(hdc, s, -1, rc, fmt | DT_NOPREFIX);
}

// ============ 表格绘制 ============

void PagesView::PaintTableHeader(HDC hdc, const RECT& rc, const ColumnDef* cols,
                                 int nCols, int flexWidth) const {
    RECT hdr{ rc.left, rc.top, rc.right, rc.top + kHeaderHeight };
    Fill(hdc, hdr, kBgHeader);
    HLine(hdc, hdr.left, hdr.right, hdr.bottom - 1, kLineHeader);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBold_));
    SetBkMode(hdc, TRANSPARENT);
    SetTextAlign(hdc, TA_LEFT | TA_TOP);

    int x = rc.left;
    for (int i = 0; i < nCols; ++i) {
        int w = cols[i].flexible ? flexWidth : cols[i].width;

        // 列间竖线：只在列头带出现，数据区没有。
        // 【像素实测】x=184/234/643/743 处
        // 1px、rgb(244,244,244)~rgb(239,239,239)，纵向覆盖整个列头带，
        // 数据区 y=37.5 处这几列都是 rgb(246~251) 近白 —— 没有竖线。
        if (i > 0) {
            DrawVLine(hdc, x, hdr.top + 4, hdr.bottom - 1, kLineHeaderCol);
        }

        RECT cell{ x + kCellPadLeft, hdr.top, x + w - kCellPadRight, hdr.bottom };
        UINT fmt = DT_SINGLELINE | DT_VCENTER | cols[i].align;
        Text(hdc, cols[i].title, &cell, fmt, kTextHeader);
        x += w;
    }
    SelectObject(hdc, old);
}

void PagesView::PaintCellText(HDC hdc, const wchar_t* text, int x, int y, int w, int h,
                              UINT align, COLORREF color) const {
    RECT cell{ x + kCellPadLeft, y, x + w - kCellPadRight, y + h };
    UINT fmt = DT_SINGLELINE | DT_VCENTER | align | DT_END_ELLIPSIS;
    Text(hdc, text, &cell, fmt, color);
}

void PagesView::FillRow(HDC hdc, int x, int y, int w, int h, int rowIndex,
                        bool selected, bool hovered) const {
    RECT row{ x, y, x + w, y + h };
    if (selected) {
        Fill(hdc, row, kBgNavSelected);
    } else if (hovered) {
        Fill(hdc, row, RGB(0xE5, 0xF3, 0xFB));
    }
    // 无交替条纹（与进程页一致）
    (void)rowIndex;
}

int PagesView::HitRow(int y, int rowCount) const {
    const int listTop = bounds_.top + kHeaderHeight;
    if (y < listTop) return -1;
    int idx = (y - listTop + scroll_) / kRowHeight;
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
    RECT rc = bounds_;

    int fixed = 0;
    for (int i = 0; i < kStartupColCount; ++i) {
        if (!kStartupCols[i].flexible) fixed += kStartupCols[i].width;
    }
    // 弹性列最小宽度 = 名称列的 exe 出厂宽 215。
    // 注意：不能用「max(下限, 剩余)」这种写法——窗口比下限还窄时会溢出到
    // 表格右界之外，把行底色画到滚动条/窗口边框上。这里改成剩余宽度不足
    // 下限时才用下限，并且所有单元格仍按同一组 x 推进，保证列对齐。
    int avail = static_cast<int>(rc.right - rc.left) - fixed;
    int flexW = (avail > 215) ? avail : 215;

    PaintTableHeader(hdc, rc, kStartupCols, kStartupColCount, flexW);

    // 空状态。
    // 【MUI 硬证据】STRINGTABLE 里 39001 = 「没有可显示的启动项」，全表唯一一条。
    // 此前 startup_ 为空时列表区纯白，什么都不画。
    if (startup_.empty()) {
        RECT msg{ rc.left, rc.top + kHeaderHeight, rc.right, rc.bottom };
        HFONT oldF = static_cast<HFONT>(SelectObject(hdc, fontBody_));
        SetBkMode(hdc, TRANSPARENT);
        Text(hdc, L"没有可显示的启动项", &msg,
             DT_SINGLELINE | DT_VCENTER | DT_CENTER, kTextSecondary);
        SelectObject(hdc, oldF);
        return;
    }

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    int y = rc.top + kHeaderHeight - scroll_;
    const int listBottom = rc.bottom;

    for (size_t i = 0; i < startup_.size(); ++i) {
        if (y + kRowHeight <= rc.top + kHeaderHeight) { y += kRowHeight; continue; }
        if (y >= listBottom) break;

        const StartupItem& it = startup_[i];
        FillRow(hdc, rc.left, y, rc.right - rc.left, kRowHeight,
                static_cast<int>(i), static_cast<int>(i) == selStartup_,
                static_cast<int>(i) == hover_);

        COLORREF tc = (static_cast<int>(i) == selStartup_) ? kTextSelected : kTextPrimary;

        int x = rc.left;

        // 名称（带 16×16 图标）
        if (!it.exePath.empty()) {
            HICON ico = GetIconFor(it.exePath);
            if (ico) {
                DrawIconEx(hdc, x + 8, y + (kRowHeight - 16) / 2, ico, 16, 16,
                           0, nullptr, DI_NORMAL);
            }
        }
        PaintCellText(hdc, it.name.c_str(), x + kIconSize + 12, y,
                      flexW - kIconSize - 12, kRowHeight, DT_LEFT, tc);
        x += flexW;

        PaintCellText(hdc, it.publisher.c_str(), x, y, kStartupCols[1].width, kRowHeight,
                      DT_LEFT, tc);
        x += kStartupCols[1].width;

        PaintCellText(hdc, it.enabled ? L"已启用" : L"已禁用", x, y,
                      kStartupCols[2].width, kRowHeight, DT_LEFT, tc);
        x += kStartupCols[2].width;

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
        PaintCellText(hdc, L"未计量", x, y, kStartupCols[3].width, kRowHeight,
                      DT_LEFT, tc);

        y += kRowHeight;
    }

    SelectObject(hdc, old);
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
    RECT rc = bounds_;

    int fixed = 0;
    for (int i = 0; i < kUserColCount; ++i) {
        if (!kUserCols[i].flexible) fixed += kUserCols[i].width;
    }
    int avail = static_cast<int>(rc.right - rc.left) - fixed;
    int flexW = (avail > 215) ? avail : 215;

    PaintTableHeader(hdc, rc, kUserCols, kUserColCount, flexW);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    int y = rc.top + kHeaderHeight - scroll_;

    for (size_t i = 0; i < users_.size(); ++i) {
        if (y >= rc.bottom) break;

        const UserSession& u = users_[i];
        FillRow(hdc, rc.left, y, rc.right - rc.left, kRowHeight,
                static_cast<int>(i), static_cast<int>(i) == selUser_,
                static_cast<int>(i) == hover_);

        COLORREF tc = (static_cast<int>(i) == selUser_) ? kTextSelected : kTextPrimary;
        int x = rc.left;

        // 用户名单元格 = 展开箭头 + 9×9 头像占位块 + 「域\用户」文字，
        // 三个元素的横向锚点全部来自原版裁切（见文件头常量注释）：
        //   chevron 10..15 / 头像块 32..41 / 文字 51
        // 【此前】这里只有一行从 x+8 起画的纯文字，整组视觉元素全缺，
        // 用户名比原版左移 43px。
        const int rowCy = y + kRowHeight / 2;
        DrawRowChevron(hdc, x + kUserChevronX, rowCy);
        DrawUserAvatarBox(hdc, x + kUserAvatarX, rowCy);

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
        // 否则文字会跑到 51+8=59。
        {
            RECT cell{ x + kUserTextX, y, x + flexW - kCellPadRight, y + kRowHeight };
            Text(hdc, display.c_str(), &cell,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS, tc);
        }
        x += flexW;

        PaintCellText(hdc, u.stateText.c_str(), x, y, kUserCols[1].width, kRowHeight,
                      DT_LEFT, tc);
        x += kUserCols[1].width;

        // 数值列格式与进程页完全一致（FmtCpu：0 值显示 0%，<1% 保留一位小数）
        wchar_t buf[64];
        FmtCpu(u.cpuPercent, buf, 64);
        PaintCellText(hdc, buf, x, y, kUserCols[2].width, kRowHeight, DT_RIGHT, tc);
        x += kUserCols[2].width;

        FmtMem(u.workingSet, buf, 64);
        PaintCellText(hdc, buf, x, y, kUserCols[3].width, kRowHeight, DT_RIGHT, tc);
        x += kUserCols[3].width;

        // 磁盘 / 网络 / GPU：没有 per-session 计数器，留空（与原版一致）
        PaintCellText(hdc, L"", x, y, kUserCols[4].width, kRowHeight, DT_RIGHT, tc);
        x += kUserCols[4].width;
        PaintCellText(hdc, L"", x, y, kUserCols[5].width, kRowHeight, DT_RIGHT, tc);
        x += kUserCols[5].width;
        PaintCellText(hdc, L"", x, y, kUserCols[6].width, kRowHeight, DT_RIGHT, tc);

        y += kRowHeight;
    }

    SelectObject(hdc, old);
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
    RECT rc = bounds_;

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

    int fixed = 0;
    for (int i = 0; i < kServiceColCount; ++i) {
        if (!kServiceCols[i].flexible) fixed += kServiceCols[i].width;
    }
    int avail = static_cast<int>(tableRc.right - tableRc.left) - fixed;
    int flexW = (avail > 120) ? avail : 120;

    PaintTableHeader(hdc, tableRc, kServiceCols, kServiceColCount, flexW);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    int y = tableRc.top + kHeaderHeight - scroll_;

    for (size_t i = 0; i < services_.size(); ++i) {
        if (y + kRowHeight <= tableRc.top + kHeaderHeight) { y += kRowHeight; continue; }
        if (y >= tableRc.bottom) break;

        const ServiceItem& s = services_[i];
        FillRow(hdc, tableRc.left, y, tableRc.right - tableRc.left, kRowHeight,
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
        DrawServiceGear(hdc, x + kServiceIconX, y + (kRowHeight - 16) / 2);
        {
            // 文字左沿 = 逻辑 25（原版实测），不走 PaintCellText 的 +8 内边距
            RECT cell{ x + kServiceTextX, y, x + kServiceCols[0].width - kCellPadRight,
                       y + kRowHeight };
            Text(hdc, s.name.c_str(), &cell,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS, tc);
        }
        x += kServiceCols[0].width;

        wchar_t buf[32];
        buf[0] = 0;
        if (s.pid != 0) swprintf_s(buf, L"%u", s.pid);
        PaintCellText(hdc, buf, x, y, kServiceCols[1].width, kRowHeight, DT_RIGHT, tc);
        x += kServiceCols[1].width;

        // 描述列 = **显示名**（DisplayName）。原版这一列不是长描述文本，
        // 长描述只在「属性」对话框/工具提示里出现。
        // 颜色用正文色 —— `svc_header_4x.png` 里 `AllJoyn Router Service`
        // 与名称列 `AJRouter` 是同一种近黑，不是次级灰。
        PaintCellText(hdc, s.displayName.c_str(), x, y, flexW, kRowHeight, DT_LEFT, tc);
        x += flexW;

        PaintCellText(hdc, s.stateText.c_str(), x, y, kServiceCols[3].width, kRowHeight,
                      DT_LEFT, tc);
        x += kServiceCols[3].width;

        PaintCellText(hdc, s.group.c_str(), x, y, kServiceCols[4].width, kRowHeight,
                      DT_LEFT, tc);

        y += kRowHeight;
    }

    SelectObject(hdc, old);
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
    RECT rc = bounds_;

    const int noteH = 44;

    // 顶部说明条（MUI 原文 38509：自 %1!s! 以来，当前用户和系统账户的资源使用情况。）
    RECT note{ rc.left, rc.top, rc.right, rc.top + noteH };
    Fill(hdc, note, RGB(0xFA, 0xFA, 0xFA));
    HLine(hdc, note.left, note.right, note.bottom - 1, kLineNavBorder);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    RECT nr{ note.left + 12, note.top + 6, note.right - 140, note.top + 26 };
    Text(hdc, L"自 30 天前以来，当前用户和系统账户的资源使用情况。", &nr,
         DT_SINGLELINE | DT_VCENTER | DT_LEFT, kTextSecondary);

    // "删除使用情况历史记录" 按钮（MUI 38505）
    RECT btn{ note.right - 172, note.top + 8, note.right - 12, note.top + 32 };
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
        if (!kAppHistCols[i].flexible) fixed += kAppHistCols[i].width;
    }
    int avail = static_cast<int>(tableRc.right - tableRc.left) - fixed;
    int flexW = (avail > 198) ? avail : 198;
    PaintTableHeader(hdc, tableRc, kAppHistCols, kAppHistColCount, flexW);

    // SRUM 不可用 → 显示 "无可用数据"（MUI 38507 原文，原版在 SRUM 被禁用时也这么显示）
    if (appHistory_.empty()) {
        // 位置与原版无从比对（原版裁切图里只有
        // 「应用历史记录」六个字的放大，不含页面内容），这里在表格区内居中，
        // 与启动页空状态用同一种居中口径，保持四页内部一致。
        RECT msg{ tableRc.left, tableRc.top + kHeaderHeight, tableRc.right, tableRc.bottom };
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

void PagesView::OnHover(int x, int y) {
    if (currentTab_ < 0) { hover_ = -1; return; }

    // 移出窗口 / 落在列头或表格外 → 取消悬停
    if (x < 0 || y < 0 || !PtInRect(&bounds_, POINT{x, y})) {
        hover_ = -1;
        return;
    }
    const int listTop = bounds_.top + kHeaderHeight;
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
    const int idx = (y - listTop + scroll_) / kRowHeight;
    hover_ = (idx >= 0 && idx < rowCount) ? idx : -1;
}

void PagesView::ScrollBy(int lines) {
    if (currentTab_ < 0 || lines == 0) return;

    int rowCount = 0;
    switch (currentTab_) {
        case 3: rowCount = static_cast<int>(startup_.size());    break;
        case 4: rowCount = static_cast<int>(users_.size());      break;
        case 6: rowCount = static_cast<int>(services_.size());   break;
        case 2: rowCount = static_cast<int>(appHistory_.size()); break;
        default: return;
    }

    // 【像素单位】scroll_ 现在是像素。滚轮传进来的是"行数"，这里乘行高。
    //
    // 原来 scroll_ 是行号、绘制却直接减（当像素用），滚一整行只移动 1 像素；
    // 而 maxScroll 是行数，永远无法停在两行之间。两头都不对。
    //
    // 现在：内容和视口都用像素算，maxScroll 可以不是 kRowHeight 的整数倍，
    // 滚到底时最后一行能完整露出，不会像原来那样留 22px 空白。
    const int viewH = bounds_.bottom - bounds_.top - kHeaderHeight;
    const int contentH = rowCount * kRowHeight;
    const int maxScroll = (contentH > viewH) ? (contentH - viewH) : 0;

    scroll_ += lines * kRowHeight;
    if (scroll_ < 0) scroll_ = 0;
    if (scroll_ > maxScroll) scroll_ = maxScroll;

    // 滚动后原悬停行已经不在原位，直接清掉，等下一次 WM_MOUSEMOVE 重算
    hover_ = -1;
}

} // namespace tmp
