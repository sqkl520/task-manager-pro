// tabs.cpp - 顶部横向标签栏实现
#include "tabs.h"
#include "theme.h"

#pragma comment(lib, "gdi32.lib")

namespace tmp {

int tabtheme::MeasureHeight(HFONT font) {
    // 页签高度 = tabtheme::kHeight = **30px @96 DPI**（UIFILE 声明 30rp，见 tabs.h）。
    //
    // 【为什么不再按字体合成】此前这里算 tmHeight + T(13) + B(4)，原因是
    // GetThemePartSize 只给出 TABITEM 边框图元 5x19，不是控件高（T_uxspec.md §2.1）。
    // 但原版 taskmgr.exe 的 DUI 资源里明确写着 `tab style height = 30rp`
    // （30001 属性 token @0x1cc），这是程序自己的声明式布局，比"用边框区拼高度"
    // 的推算法权威得多。故改为直接采用 30。
    //
    // 【DPI】30rp 必须自己乘 dpi/96 —— 主题的 TABITEM 尺寸不随 DPI 变化
    // （Aero 只有 NormalSize，R_sysmetrics.md §5.4）。本机 96 DPI 下
    // theme::Scaled(30) 恒等于 30，与改动前逐点等价；125% 下才是 37。
    const int baseH = theme::Scaled(tabtheme::kHeight);
    //
    // 【只有一个例外】若页签字体大到 baseH 装不下（tmHeight > baseH），才按字体撑高，
    // 否则会裁字。这是纯粹的防裁字保护，不是"按字体重算高度"。
    //   参考：12px YaHei tmHeight=17；13px → 19；要超过 30 得 21px 以上字号。
    int tmHeight = 0;
    if (font) {
        HDC dc = GetDC(nullptr);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, font);
            TEXTMETRICW tm{};
            if (GetTextMetricsW(dc, &tm)) tmHeight = tm.tmHeight;
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
        }
    }
    if (tmHeight > baseH) return tmHeight;
    return baseH;
}

// 【悬停底色已上移到 tabtheme::kHoverBg】旧实现在这里有一个匿名 namespace 里
// 的 kHoverBg = #F7F7F7，是自创值（无任何来源，TABITEM s2 实测是 #D8EAF9）。
// 常量已并入 tabs.h 的 tabtheme，此处不再重复定义。

void TabBar::SetBounds(int x, int y, int w, int h) {
    bounds_ = RECT{ x, y, x + w, y + h };
    widthsValid_ = false;
}

void TabBar::SetCurrent(TabId t) {
    if (t == current_) return;
    current_ = t;
}

void TabBar::RecomputeWidths(HDC hdc) {
    widths_.assign(static_cast<size_t>(TabId::Count), 0);
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font_));
    for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
        const wchar_t* t = TabTitle(static_cast<TabId>(i));
        SIZE sz{};
        GetTextExtentPoint32W(hdc, t, static_cast<int>(wcslen(t)), &sz);
        // 宽度 = 文字 advance + 左右内边距各 kPadX。
        // 【规范未规定页签宽度】comctl32 的 tab 宽度算法未文档化；这里用
        // 「文字 + 对称内边距」这一最保守的定义。kPadX=11 取 Win32 官方间距集
        // 的 11 rp（对话框边距 / 不相关控件间距，见 T_uxspec.md §7.2）。
        // 【DPI】11rp 要自己乘 dpi/96（主题度量不随 DPI 变，R_sysmetrics.md §5.4）。
        // 本机 96 ⇒ theme::Scaled(11) == 11，与改动前完全等价。
        widths_[i] = sz.cx + theme::Scaled(tabtheme::kPadX) * 2;
    }
    SelectObject(hdc, old);
    widthsValid_ = true;
}

TabId TabBar::HitTest(int x, int y) const {
    if (y < bounds_.top || y >= bounds_.bottom) return TabId::Count;
    if (x < bounds_.left || x >= bounds_.right) return TabId::Count;
    if (widths_.empty()) return TabId::Count;

    int cx = bounds_.left;
    for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
        int w = widths_[i];
        if (x >= cx && x < cx + w) return static_cast<TabId>(i);
        cx += w;
    }
    return TabId::Count;
}

void TabBar::Paint(HDC hdc) const {
    if (widths_.empty()) {
        const_cast<TabBar*>(this)->RecomputeWidths(hdc);
    }

    // 底色 = TABBODY 渲染值 #FFFFFF（系统API实测，T_uxspec.md §2.3）
    HBRUSH bg = CreateSolidBrush(tabtheme::kBg);
    FillRect(hdc, &bounds_, bg);
    DeleteObject(bg);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, font_));
    SetBkMode(hdc, TRANSPARENT);

    // ========================================================================
    // 页签样式：**每个页签是独立的带边框方块**。
    //
    // 【依据：Win10 规范确认，四条系统 API 实测证据】
    //   1. 主题为 TABITEM 提供四向可拉伸边框图元（TABITEMLEFTEDGE /
    //      TABITEMRIGHTEDGE / TABITEMBOTHEDGE / TOPTABITEM* 共 8 个变体）。
    //      下划线方案只需要一条横线，不需要左右边缘 part。
    //   2. TABITEM 的 TMT_SIZINGMARGINS = L2 R2 T13 B4（右侧 2px 是可拉伸
    //      边框带），说明存在独立的边框结构。
    //   3. 离屏渲染（DrawThemeBackground → 32bpp DIB → 像素取样）：
    //        s1 TIS_NORMAL   #F0F0F0 填充 + #D9D9D9 边框
    //        s2 TIS_HOT      #D8EAF9 填充 + #D9D9D9 边框
    //        s3 TIS_SELECTED #FFFFFF 填充 + #D9D9D9 边框
    //      是「一块有边界的面」，不是「一条线」。
    //   4. TOPTABITEM 与 TABITEM 度量完全相同（5x19 / L2 R2 T13 B4）。
    //   出处 docs/specs/T_uxspec.md §2.4、U_themecolors.md §4.4
    //
    // 【TMT_BORDERSIZE = 0 的处理】主题的边框是位图画出来的，没有 1px 描边。
    // 我们自绘时用 1px 实线代替：这是**实现手段的简化**，不是对规范的偏离。
    // 1px 是 Win32 的 kBorderWidth（SM_CYBORDER = 1，不随 DPI 缩放），
    // 与 #D9D9D9 配合后的观感与 aero 位图边框一致。
    //
    // 【历史教训，保留以免重犯】曾依据 m3.png 把这里改成
    // 「纯白底 + 活动页签 2px 蓝下划线」，并断言「带边框方块是旧形态」。
    // m3.png 实为本项目自己旧版本的运行截图（底部有 tmwindow_list.cpp 独有的
    // 「进程数: 562 …」状态栏，MUI 里没有这个词；蓝下划线也是我方旧版自己画的）。
    // 下划线是 WinUI/UWP Pivot 的范式，不是 Win32 TabControl 的范式。
    // ========================================================================
    int cx = bounds_.left;
    for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
        int w = widths_[i];
        bool on = (static_cast<int>(current_) == i);
        bool hov = (static_cast<int>(hover_) == i);

        // 【活动页签比非活动页签高 2px —— 真原版实测】
        // 原版 docs/reference/original_win10.png：
        //   活动页签「进程」顶边 y=54，非活动页签（性能/用户/…）顶边 y=56
        //   底边都在 y=78，但活动页签底部不封口（见下面的边框画法）
        // 即活动页签向上凸出 2px，覆盖住页签带上沿的那条分隔。
        // 这是标签页"当前项跳出"的视觉语言，也是"层次感"最直观的来源。
        //
        // 此前所有页签顶边都是 bounds_.top，高度完全一样，
        // 看起来像一排平齐的按钮而不是标签页。
        const int raise = on ? tabtheme::kActiveRaise : 0;
        RECT tabRc{ cx, bounds_.top - raise, cx + w, bounds_.bottom };

        // 底色三态：活动 #FFFFFF / 悬停 #D8EAF9 / 非活动 #F0F0F0
        // 全部来自 TABITEM s3/s2/s1 的离屏渲染取样（系统API实测）。
        // 悬停优先于非活动：鼠标所在的那个页签显示 hot 态。
        COLORREF fill = on ? tabtheme::kActiveBg
                      : hov ? tabtheme::kHoverBg
                            : tabtheme::kInactiveBg;
        HBRUSH hb = CreateSolidBrush(fill);
        FillRect(hdc, &tabRc, hb);
        DeleteObject(hb);

        // 1px 边框（#D9D9D9，TABITEM 全状态实测值）
        //
        // 【层次感的关键 —— 真原版截图实测】
        // 原版的活动页签**底部不画边框**，它与下方内容区连成一体，
        // 看起来像"打开了的标签页"；非活动页签四周都有边框，被"压在"底线之下。
        //
        // 实测（docs/reference/original_win10.png，y=54..78）：
        //   活动页签「进程」x=1..40：顶/左/右边框有，**底边没有**
        //   y=78 那条贯穿的底线 #D9D9D9 **从 x=41 才开始**，
        //   即在活动页签正下方是断开的（否则活动页签也被"关"上了）
        //   非活动页签 x=41 起：四周全有边框
        //
        // 此前我方给每个页签画完整 Rectangle()，连活动页签也有底边、
        // 整行底线也不断开 —— 于是所有页签长得一样，没有"当前页"的层次。
        HPEN pen = CreatePen(PS_SOLID, 1, tabtheme::kBorder);
        HGDIOBJ oldp = SelectObject(hdc, pen);
        HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        if (on) {
            // 活动页签：画顶边 + 左右边，底边留空。
            // 用 Polyline 画三条线（缺底边），而不是 Rectangle。
            POINT pts[5] = {
                { tabRc.left,       tabRc.bottom - 1 },   // 左下
                { tabRc.left,       tabRc.top },          // 左上
                { tabRc.right - 1,  tabRc.top },          // 右上
                { tabRc.right - 1,  tabRc.bottom - 1 },   // 右下
            };
            // 只画前三条边：左、顶、右
            Polyline(hdc, pts, 3);          // 左下→左上→右上
            MoveToEx(hdc, tabRc.right - 1, tabRc.top, nullptr);
            LineTo(hdc, tabRc.right - 1, tabRc.bottom);
        } else {
            Rectangle(hdc, tabRc.left, tabRc.top, tabRc.right, tabRc.bottom);
        }
        SelectObject(hdc, oldb);
        SelectObject(hdc, oldp);
        DeleteObject(pen);

        // 文字：活动/非活动同色 COLOR_BTNTEXT(#000000)。
        // 主题对 TABITEM 未定义 TMT_TEXTCOLOR（实测返回 na），所以规范上只能
        // 取系统 COLOR_BTNTEXT。非活动页签**不是禁用**，因此不用
        // COLOR_GRAYTEXT(#6D6D6D) —— 那会让它看起来不可点。
        // 活动/非活动的区分交给底色（#FFFFFF vs #F0F0F0），与 aero 一致。
        SetTextColor(hdc, on ? tabtheme::kTextOn : tabtheme::kTextOff);
        RECT tr = tabRc;
        DrawTextW(hdc, TabTitle(static_cast<TabId>(i)), -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

        cx += w;
    }

    // 收边线：把页签带与内容区分开。
    //
    // 【真原版实测：这条线在活动页签下方是断开的】
    // 原版 docs/reference/original_win10.png 的 y=78 一整行：
    //   x=1..40（活动页签「进程」正下方）→ 白色，**没有线**
    //   x=41..（非活动页签 + 右侧留白）  → #D9D9D9 有线
    // 这正是"标签页"的视觉语言：当前页与内容区连通，其余页被底线封住。
    //
    // 此前我方从 bounds_.left 一路画到 bounds_.right，把活动页签也封死了，
    // 于是所有页签看起来一样高、一样封闭 —— 这是"没有层次"的根因之一。
    {
        HPEN pen = CreatePen(PS_SOLID, 1, tabtheme::kBottomLine);
        HGDIOBJ oldp = SelectObject(hdc, pen);
        const int lineY = bounds_.bottom - 1;

        int x = bounds_.left;
        int activeL = -1, activeR = -1;
        for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
            const int w = widths_[i];
            if (static_cast<int>(current_) == i) {
                activeL = x;
                activeR = x + w;
                break;
            }
            x += w;
        }

        if (activeL < 0) {
            MoveToEx(hdc, bounds_.left, lineY, nullptr);
            LineTo(hdc, bounds_.right, lineY);
        } else {
            if (activeL > bounds_.left) {
                MoveToEx(hdc, bounds_.left, lineY, nullptr);
                LineTo(hdc, activeL, lineY);
            }
            MoveToEx(hdc, activeR, lineY, nullptr);
            LineTo(hdc, bounds_.right, lineY);
        }

        SelectObject(hdc, oldp);
        DeleteObject(pen);
    }

    // 【为什么页签之间不需要额外的竖线】
    // 每个页签的 Rectangle() 自己画了四条边（GDI 的 Rectangle 从 right-1 /
    // bottom-1 收口），相邻两个页签的右/左边相邻 → 视觉上是 2px 的分隔带，
    // 与 aero 的 TABITEMBOTHEDGE（左右各 2px 边框带）在量级上一致。
    // 所以不需要再单独画分隔线，也不需要 tabtheme::kLine（那个值属于列头）。

    SelectObject(hdc, old);
}

} // namespace tmp
