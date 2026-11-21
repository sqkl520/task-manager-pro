// pages_view.h - 四个次要页面的绘制
//
// 统一表格样式（与进程页一致）：
//   行高 32px / 列头 24px 白底 / 无交替条纹 / 无网格线
//   行悬停 #E5F3FB / 行选中 #CCE8FF / 图标 16x16 左内边距 8px
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
    // ScrollBy 以「行」为单位（正数向下），内部按当前页的行数 clamp，
    // 不做像素级滚动 —— 调用方用 -delta/WHEEL_DELTA 换算。
    void ScrollBy(int lines);
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
    void PaintCellText(HDC hdc, const wchar_t* text, int x, int y, int w, int h,
                       UINT align, COLORREF color) const;
    void FillRow(HDC hdc, int x, int y, int w, int h, int rowIndex,
                 bool selected, bool hovered) const;

    int HitRow(int y, int rowCount) const;   // 返回行索引或 -1

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
    int scroll_ = 0;
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

    HFONT fontBody_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontTitle_ = nullptr;

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
