// pages_view.h - 四个次要页面的绘制
//
// 统一表格样式（与进程页一致）：
//   行高 28rp / 列头 24rp 白底 / 无交替条纹 / 无网格线
//   行悬停 #E5F3FB / 行选中 #CCE8FF / 图标 16rp 左内边距 8rp
//
// 【单位】上面和下面所有几何值都是 **96 DPI 基准**（= 原版 UIFILE 的 rp 值，
// 本机 100% 时 1rp = 1px）。主题 part 尺寸不随 DPI 变化（Aero.msstyles 只有
// NormalSize 变体），所以绘制与命中测试处必须自己经 theme::Rpx() 换算，
// 不许把 28 / 24 / 16 这些数字直接当像素用。
#pragma once

#include "pages_data.h"
#include "theme.h"
#include "snapshot.h"

#include <windows.h>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

namespace tmp {

class PagesView {
public:
    ~PagesView();

    void BuildFonts();
    // 字体是按「建字体时的 Dpi()」算死的（lfHeight = -Rpx(px)）。窗口换到另一块
    // 缩放不同的显示器后 Dpi() 会变，已建好的 HFONT 不会自己变大，于是行高放大了
    // 字还是旧的。绘制入口每次先调它：DPI 与建字体时不一致就重建。
    void EnsureFonts() const;

    // WM_DPICHANGED 时调用，oldDpi = 变更前的 DPI。
    //
    // 【为什么需要】scroll_ 是**像素**偏移，不是行号。DPI 变了以后
    // 内容总高按比例变大，而 scroll_ 若原样保留，同一个像素值在新布局里
    // 指向的是另一行 —— 用户会看到画面突然跳到约 2/3 的位置。这里把偏移
    // 按 DPI 比例折算，保住"停在同一个位置"。
    //
    // 【调用时机】必须在 theme::SetDpi(newDpi) **之后**、下一次绘制之前。
    // TmWindow 的 WM_DPICHANGED 已经调了 SetDpi，所以放在 BuildLayout() 旁边即可。
    // 不调也不会崩，只是切换显示器时滚动位置会跳。
    void OnDpiChanged(int oldDpi);

    // ---- DPI 自测（供 main.cpp 的 --selftest 调用）----
    //
    // 返回空 = 全部通过；否则返回第一条失败项的说明。
    // 内部会临时改 theme::SetDpi()，退出时恢复调用前的 DPI —— 调用方不需要
    // 也不应该自己再恢复一次。
    //
    // 断言内容：
    //   1) Rpx(28) 在 96/120/144/192 下分别等于 28/35/42/56
    //   2) Rpx(180)（服务页名称列）在同样四个 DPI 下的绝对值
    //   3) 一个具体列宽在 144/96 下的比值 == 1.5（整数交叉相乘，不用浮点）
    //   4) 10 个几何常量在 96/144 下逐个等比
    //   5) 弹性列布局自洽：列宽之和 == 表格宽（任何 DPI 下都成立）
    //   6) 字体 lfHeight 走 Rpx()，且 DPI 变化后 EnsureFonts() 真的重建
    //   7) 文字宽度取自 GetTextExtentPoint32W，随字体一起变大（不得二次缩放）
    //   8) 真渲染回读：把 PaintServices 画进离屏 DIB，验列头底线的像素行
    //   9) 滚动偏移的 DPI 折算（RescaleScrollPx 的算术 + 对象层幂等/不越界）
    static std::wstring SelfTestDpi();

    void SetBounds(const RECT& rc) { bounds_ = rc; }
    const RECT& Bounds() const { return bounds_; }

    // 数据
    void SetSnapshot(std::shared_ptr<const Snapshot> snap) { snap_ = std::move(snap); }
    void RefreshStartup();
    void RefreshUsers();
    void RefreshServices();
    void RefreshAppHistory();

    // 绘制（每页一个入口）
    void PaintStartup(HDC hdc) const;
    void PaintUsers(HDC hdc) const;
    void PaintServices(HDC hdc) const;
    void PaintAppHistory(HDC hdc) const;

    // 交互（返回 true 表示已处理）
    bool OnClickStartup(int x, int y);
    bool OnClickUsers(int x, int y);
    bool OnClickServices(int x, int y);

    // 选中项索引（-1 = 无）
    int SelectedStartup() const { return selStartup_; }
    int SelectedService() const { return selService_; }
    int SelectedUser() const { return selUser_; }

    const std::vector<StartupItem>& StartupItems() const { return startup_; }
    const std::vector<ServiceItem>& ServiceItems() const { return services_; }
    // 用户会话列表：右键菜单的断开/注销/发消息要用 sessionId
    const std::vector<UserSession>& UserItemsForCommand() const { return users_; }

    bool ToggleStartup(int index);
    bool StartService(int index);
    bool StopService(int index);
    // 重启服务：先异步停、记下名字，下一轮 RefreshServices 发现它停了再启动。
    // 【为什么不在窗口过程里 Sleep】那条路径在 UI 线程上同步执行，Sleep(600)
    // 会让整个窗口 0.6 秒不处理消息，标题栏直接显示「(未响应)」。
    void RestartServiceAsync(int index);

    // 服务描述/组是渐进填充的：每次 Fill 调用补一批（规格实测每个 2.8ms，
    // 306 个全补要 857ms，绝不能一次做完）
    // 返回 true 表示还有未填充的项，调用方应继续触发
    bool EnrichServicesBatch(int maxCount = 8);
    bool ServicesNeedEnrich() const { return enrichCursor_ < static_cast<int>(services_.size()); }

    // 悬停与滚动。两者都必须由 TmWindow 的事件层调用，否则 hover_ / scroll_
    // 永远是初始值 —— 行悬停不会出现、服务页 300 多项只有前十几行可达。
    // OnHover 传客户区坐标，内部换算成当前页的行号；(-1,-1) 表示移出。
    void OnHover(int x, int y);
    // 清空悬停态，返回是否真的改变了（调用方据此决定要不要重绘）。
    // 鼠标移出窗口时由 TmWindow::ClearAllHover 调用：那时不会再有任何
    // WM_MOUSEMOVE / OnHover 进来，不显式清就会永久保持最后一行的高亮。
    bool ClearHover() {
        if (hover_ < 0) return false;
        hover_ = -1;
        return true;
    }
    // 光标判定用：该坐标是否落在当前页的数据行上（表头不算、空白不算）。
    // 返回行索引或 -1。与 HitRow 的区别是它自己取当前页的行数，
    // 调用方不用知道 pages 的内部结构。
    int HitRowAt(int x, int y) const;
    // 表头命中：返回列索引或 -1。坐标落在表头带内且在某列范围里才算。
    int HitHeaderAt(int x, int y) const;
    // 点表头：同列再点一次翻转方向，换列则重置为升序。
    // 返回 true 表示排序状态变了（调用方需要重绘）。
    bool ClickHeader(int col);
    // 按当前页的排序状态重排数据
    void ApplySort();
    // ScrollBy 以「行」为单位（正数向下），内部按当前页的行数 clamp。
    // 它只是 ScrollByPixels 的一层换算，真正的状态是像素。
    void ScrollBy(int lines);

    // ---- 像素级滚动（滚动系统用）----
    //
    // 【为什么必须开这三个口子】统一的滚动系统（TMWindow 的 ScrollTarget）
    // 要让「滚轮 / 触控板 / 触屏 / 滚动条」四套输入改同一份像素状态，
    // 而 scroll_ 是私有的。只给行号接口的话，惯性每帧只能走整数行，
    // 亚像素速度会被四舍五入成 0 —— 阻尼滑行在最后一屏就卡住不动。
    // 这三个函数是「精确位移」的唯一入口，行号接口是它的语法糖。
    void ScrollByPixels(int px);
    void SetScrollPixels(int px);
    int  ScrollPixels() const { return scroll_; }
    // 当前页可滚到的最大像素偏移（内容高 - 视口高，不小于 0）。
    // 无表格的页（性能页等）返回 0。
    int  MaxScrollPixels() const;
    // 当前显示的是哪一页。OnHover/ScrollBy 需要它才能知道该对哪张表操作，
    // 由 TmWindow 在切页与绘制时同步。
    void SetCurrentTab(int tabId) { currentTab_ = tabId; }

private:
    // 通用表格绘制
    struct ColumnDef {
        const wchar_t* title;
        int width;
        UINT align;
        bool flexible;
    };

    void PaintTableHeader(HDC hdc, const RECT& rc, const ColumnDef* cols, int nCols,
                          int flexWidth) const;
    // 排序指示器：desc=true 画 ▼，false 画 ▲。9 行高的细折角。
    void DrawSortChevron(HDC hdc, int cx, int cy, bool desc, COLORREF color) const;
    // 当前页的列定义 + 弹性宽度基准。nullptr = 该页没有表格。
    const ColumnDef* CurCols(int& nCols, int& flexW) const;
    void PaintCellText(HDC hdc, const wchar_t* text, int x, int y, int w, int h,
                       UINT align, COLORREF color) const;
    void FillRow(HDC hdc, int x, int y, int w, int h, int rowIndex,
                 bool selected, bool hovered) const;

    int HitRow(int y, int rowCount) const;   // 返回行索引或 -1

    // 当前页的数据行数与视口高度（像素）。ScrollByPixels / MaxScrollPixels
    // 和 OnHover / HitRowAt 共用同一份口径 —— 四处各写一遍必然漂移。
    int CurRowCount() const;
    int CurViewHeight() const;

    void Text(HDC hdc, const wchar_t* s, RECT* rc, UINT fmt, COLORREF c) const;
    void Fill(HDC hdc, const RECT& rc, COLORREF c) const;
    void HLine(HDC hdc, int x1, int x2, int y, COLORREF c) const;

    RECT bounds_{};
    std::shared_ptr<const Snapshot> snap_;

    std::vector<StartupItem> startup_;
    std::vector<UserSession> users_;
    std::vector<ServiceItem> services_;
    std::vector<AppHistoryItem> appHistory_;

    int selStartup_ = -1;
    int selUser_ = -1;
    int selService_ = -1;
    int hover_ = -1;
    // 【单位是像素，不是行号 · 2026-11-21】
    //
    // 原来这里是行号单位，但绘制侧写的是 `kHeaderHeight - scroll_`（把行号
    // 当像素减），滚一整行只移动 1 像素 —— 滚轮拨半天画面纹丝不动。
    // 而 maxScroll 又是行数，永远无法表达"停在两行之间"。
    //
    // 改成像素后三件事同时正确：绘制直接减、滚轮按行高累加、
    // maxScroll = 内容总高 - 可视高（可以不是行高的整数倍）。
    // 副作用是滚到底时最后一行能完整露出，不会留下 22px 的空白。
    int scroll_ = 0;
    // 排序状态（四个次要页共用一套字段，按页记住各自的）。
    //
    // 【为什么是数组】四个页的列定义各不相同（启动 4 列、用户 7 列、服务 3 列、
    // 应用历史 3 列），排序状态必须按页分开——否则在服务页按「状态」排完
    // 切到用户页，那个列号会指到不相干的列上。
    //
    // kMaxSortPages 取 8，够覆盖 TabId 的全部取值。
    static constexpr int kMaxSortPages = 8;
    int sortCol_[kMaxSortPages]  = { -1, -1, -1, -1, -1, -1, -1, -1 };
    bool sortDesc_[kMaxSortPages] = { false, false, false, false, false, false, false, false };
    // 当前页的排序列 / 方向（-1 = 不排序，保持数据源顺序）
    int CurSortCol() const {
        return (currentTab_ >= 0 && currentTab_ < kMaxSortPages) ? sortCol_[currentTab_] : -1;
    }
    bool CurSortDesc() const {
        return (currentTab_ >= 0 && currentTab_ < kMaxSortPages) ? sortDesc_[currentTab_] : false;
    }
    int currentTab_ = -1;   // TabId 的值，-1 = 还没同步过
    int enrichCursor_ = 0;
    // 「重启服务」第二阶段用：待启动的服务短名。非空表示上一轮已发出停止，
    // 正在等它真的停下来。见 PagesView::RestartServiceAsync。
    std::wstring pendingRestart_;

    // 图标缓存（按 exe 路径）
    //
    // 【有这个上限的原因】原来这张表只在 ~PagesView() 里 DestroyIcon，
    // 运行期间永不淘汰。用户机器 600+ 进程，长跑过程中不断有新程序启动，
    // 每个新 exe 路径就多一个常驻 HICON（16×16 位图 + 图标数据，约几 KB
    // 到几十 KB）。**这是「长时间运行变卡」里唯一确认会无界增长的内存项。**
    // 加个上限，超了就把整张表清空重建 —— 重建成本是每个可见行一次
    // SHGetFileInfoW（几毫秒），而这张表本身只有几百个条目、命中率极高，
    // 所以偶尔清空一次对体验没有可感影响。
    //
    // 取值依据：本机进程数 640 左右，但不同 exe 路径通常只有一百多个
    // （大量进程共享同一 exe）。512 给足余量，又不会让内存无限涨。
    static constexpr size_t kIconCacheMax = 512;
    mutable std::unordered_map<std::wstring, HICON> iconCache_;

    // 【mutable 的原因】EnsureFonts() 是 const 的（四个 Paint* 都是 const），
    // 但它要在 DPI 变化后就地重建这四个字体。它们对使用者仍然只读：
    // 外部没有任何写入口，只有 EnsureFonts()/BuildFonts() 会改。
    mutable HFONT fontBody_ = nullptr;
    mutable HFONT fontBold_ = nullptr;
    mutable HFONT fontSmall_ = nullptr;
    mutable HFONT fontTitle_ = nullptr;
    // 建上面四个字体时的 DPI。与当前 Dpi() 不等时 EnsureFonts() 会重建。
    // 0 = 还没建过。
    mutable int fontsDpi_ = 0;

    HICON GetIconFor(const std::wstring& path) const;

    // 各页列定义
    static const ColumnDef kStartupCols[];
    static const int kStartupColCount;
    static const ColumnDef kUserCols[];
    static const int kUserColCount;
    static const ColumnDef kServiceCols[];
    static const int kServiceColCount;
    static const ColumnDef kAppHistCols[];
    static const int kAppHistColCount;
};

} // namespace tmp
