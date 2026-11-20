// tableview.h - 自绘虚拟化表格控件
//
// 设计依据 (TECH_STACK.md Top 1 / System Informer 的 treenew.c)：
//   - 只遍历 [firstRow, lastRow] 可见区间，与总行数无关
//   - GetUpdateRgn 脏区裁剪 + 双缓冲 BitBlt
//   - 纯 GDI 文字渲染，CJK/IME 零适配
//   - 每帧目标 < 4ms
#pragma once

#include "snapshot.h"
#include <windows.h>
#include <vector>
#include <string>
#include <functional>

namespace tmp {

// 列定义
// getText 直接写入调用方提供的栈缓冲区，避免每帧数百次 std::wstring 堆分配。
// 这是本控件与"教科书式"实现的关键差异：10000 行 × 8 列 × 每秒刷新会产生
// 巨量临时字符串，堆分配本身就能吃掉整个帧预算。
struct Column {
    const wchar_t* title;
    int width;
    int align;                 // DT_LEFT / DT_RIGHT
    void (*getText)(const ProcessEntry&, wchar_t* buf, size_t cch);
    std::function<double(const ProcessEntry&)> getSortKey;   // 可空
};

class TableView {
public:
    TableView();
    ~TableView();

    bool Create(HWND parent, int id);
    HWND Handle() const { return hwnd_; }

    // 发布新数据（UI 线程调用，只存指针，不拷贝）
    void SetData(const Snapshot* snap);

    void Invalidate();

    // 自监控指标
    int LastPaintMicros() const { return lastPaintMicros_; }
    int VisibleRowCount() const { return lastVisibleRows_; }
    // 累计统计（供基准测试用，避免时序竞态）
    uint64_t PaintCount() const { return paintCount_; }
    uint64_t TotalPaintMicros() const { return totalPaintMicros_; }
    void ResetPaintStats() { paintCount_ = 0; totalPaintMicros_ = 0; }

    // 分段耗时（微秒），用于定位瓶颈
    int ProfFill() const { return profFill_; }
    int ProfRows() const { return profRows_; }
    int ProfHead() const { return profHead_; }
    int ProfBlit() const { return profBlit_; }

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void EnsureBackBuffer(int w, int h);
    void Paint(HDC hdc, const RECT& paintRect);
    void PaintRows(HDC hdc, int firstRow, int lastRow, int firstCol, int lastCol);
    void PaintHeader(HDC hdc, int firstCol, int lastCol);
    void RebuildColumns();
    void RebuildView();               // 排序 + 建立可见索引
    void UpdateScrollBars();

    int RowFromY(int y) const;
    int ColFromX(int x) const;
    int TotalContentHeight() const;
    int TotalContentWidth() const;

    HWND hwnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP memBmp_ = nullptr;
    int memW_ = 0, memH_ = 0;

    const Snapshot* snap_ = nullptr;

    std::vector<Column> columns_;
    std::vector<int> colOffsets_;

    // 视图（排序后的行索引）
    std::vector<size_t> view_;
    int sortColumn_ = 2;              // 默认按 CPU 降序
    bool sortDesc_ = true;

    int scrollY_ = 0;
    int scrollX_ = 0;

    int rowHeight_ = 20;
    int headerHeight_ = 24;

    HFONT font_ = nullptr;
    HFONT fontBold_ = nullptr;

    // 缓存 GDI 对象：绝不在 WM_PAINT 里创建/销毁
    HBRUSH brBg_ = nullptr;
    HBRUSH brAlt_ = nullptr;
    HBRUSH brHeader_ = nullptr;
    HPEN   penGrid_ = nullptr;

    int lastPaintMicros_ = 0;
    int lastVisibleRows_ = 0;
    uint64_t paintCount_ = 0;
    uint64_t totalPaintMicros_ = 0;

    int profFill_ = 0;
    int profRows_ = 0;
    int profHead_ = 0;
    int profBlit_ = 0;

    // 后备缓冲刚重建时必须全屏重绘一次，否则会出现未初始化的区域
    bool forceFullRepaint_ = true;
};

} // namespace tmp
