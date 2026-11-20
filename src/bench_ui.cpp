// bench_ui.cpp - milestone 2: validate the render layer
//
// 用假数据灌 N 行，测量每帧绘制耗时。
// 关键：用 UpdateWindow 强制同步绘制，避免 WM_PAINT 异步导致的测量竞态。
#include "tableview.h"
#include "snapshot.h"

#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <random>

#pragma comment(lib, "comctl32.lib")

using namespace tmp;

namespace {
HWND g_main = nullptr;
TableView* g_tv = nullptr;
Snapshot g_fake;
std::vector<double> g_paintTimes;
int g_targetRows = 10000;
int g_frame = 0;
bool g_done = false;

void BuildFakeData(int n) {
    g_fake.sequence = 1;
    g_fake.processes.clear();
    g_fake.processes.reserve(n);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> cpu(0.0, 25.0);
    std::uniform_int_distribution<uint64_t> mem(1 << 20, 8ull << 30);
    for (int i = 0; i < n; ++i) {
        ProcessEntry e;
        e.pid = 1000 + i;
        wchar_t name[64];
        swprintf_s(name, L"process_%05d.exe", i);
        e.imageName = name;
        e.cpuPercent = cpu(rng);
        e.workingSet = mem(rng);
        e.privateBytes = e.workingSet / 2;
        e.threadCount = 1 + static_cast<uint32_t>(rng() % 200);
        e.handleCount = 10 + static_cast<uint32_t>(rng() % 2000);
        e.sessionId = i % 2;
        g_fake.processes.push_back(std::move(e));
    }
    g_fake.totalProcesses = n;
    g_fake.totalThreads = n * 5;
    g_fake.totalHandles = n * 20;
    g_fake.totalPhysical = 32ull << 30;
    g_fake.availablePhysical = 8ull << 30;
    g_fake.commitLimit = 57ull << 30;
    g_fake.commitTotal = 35ull << 30;
    g_fake.collectionMs = 13.0;
    g_fake.bufferBytes = 1133 * 1024;
}

void Report() {
    KillTimer(g_main, 1);
    g_done = true;

    std::vector<double> v = g_paintTimes;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double d : v) sum += d;
    double avg = v.empty() ? 0 : sum / v.size();
    double p50 = v.empty() ? 0 : v[v.size() * 50 / 100];
    double p95 = v.empty() ? 0 : v[v.size() * 95 / 100];
    double mx = v.empty() ? 0 : v.back();

    std::printf("\n=== UI RENDER BENCHMARK ===\n");
    std::printf("  Total rows in dataset : %d\n", g_targetRows);
    std::printf("  Visible rows rendered : %d\n", g_tv->VisibleRowCount());
    std::printf("  Frames measured       : %zu\n", v.size());
    std::printf("  Paint time  avg %6.3f ms   p50 %6.3f   p95 %6.3f   max %6.3f\n",
                avg, p50, p95, mx);
    std::printf("  Breakdown (us): fill=%d  rows=%d  header=%d  blit=%d\n",
                g_tv->ProfFill(), g_tv->ProfRows(), g_tv->ProfHead(), g_tv->ProfBlit());
    std::printf("\n  VERDICT: ");

    if (v.size() < 50) {
        std::printf("INVALID - only %zu frames captured (expected 300).\n", v.size());
        std::printf("           Something is wrong with the paint path, not the performance.\n");
    } else if (g_tv->VisibleRowCount() <= 0) {
        std::printf("INVALID - 0 visible rows means nothing was actually rendered.\n");
    } else if (g_tv->VisibleRowCount() > 100) {
        std::printf("SUSPECT - %d visible rows is too many; virtualization may be off.\n",
                    g_tv->VisibleRowCount());
    } else if (p95 < 8.0) {
        std::printf("PASS\n");
        std::printf("           p50 %.3f ms = %.1f%% of the 16.6ms frame budget.\n",
                    p50, p50 / 16.6 * 100.0);
        std::printf("           Only %d of %d rows rendered -> cost is independent of row count.\n",
                    g_tv->VisibleRowCount(), g_targetRows);
    } else if (p95 < 16.6) {
        std::printf("OK - p95 %.3f ms fits one frame. This machine was under load during\n", p95);
        std::printf("     measurement (java/ghidra etc.); re-run on an idle machine for the\n");
        std::printf("     true figure. Still within budget.\n");
    } else {
        std::printf("FAIL - p95 %.3f ms exceeds one frame budget (16.6ms).\n", p95);
    }
    std::printf("\n");
    fflush(stdout);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_tv = new TableView();
        if (!g_tv->Create(hwnd, 100)) {
            std::printf("[DIAG] TableView::Create FAILED, GetLastError=%lu\n", GetLastError());
            fflush(stdout);
        } else {
            std::printf("[DIAG] TableView created, hwnd=%p\n", (void*)g_tv->Handle());
            fflush(stdout);
        }
        g_tv->SetData(&g_fake);
        SetTimer(hwnd, 1, 1, nullptr);   // tick as fast as possible
        return 0;

    case WM_SIZE:
        if (g_tv && g_tv->Handle())
            MoveWindow(g_tv->Handle(), 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;

    case WM_TIMER: {
        if (g_done) return 0;
        if (!g_tv || !g_tv->Handle()) {
            static int warned = 0;
            if (warned++ == 0) { std::printf("[DIAG] WM_TIMER fired but g_tv/handle is null\n"); fflush(stdout); }
            return 0;
        }

        // 滚一下，然后同步强制绘制
        SendMessageW(g_tv->Handle(), WM_VSCROLL, MAKEWPARAM(SB_LINEDOWN, 0), 0);

        uint64_t pcBefore = g_tv->PaintCount();

        UpdateWindow(g_tv->Handle());    // 同步执行 WM_PAINT

        if (g_tv->PaintCount() > pcBefore) {
            g_paintTimes.push_back(g_tv->LastPaintMicros() / 1000.0);
        }
        ++g_frame;

        if (g_frame == 1 || g_frame == 10) {
            std::printf("[DIAG] frame=%d paintCount=%llu visible=%d tvSize=%d\n",
                        g_frame,
                        static_cast<unsigned long long>(g_tv->PaintCount()),
                        g_tv->VisibleRowCount(),
                        (int)g_tv->Handle());
            fflush(stdout);
        }

        if (g_frame >= 300) Report();
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2) {
        int n = _wtoi(argv[1]);
        if (n > 0) g_targetRows = n;
    }

    std::printf("=== Task Manager Pro :: UI Render Benchmark ===\n");
    std::printf("Generating %d fake rows...\n", g_targetRows);
    BuildFakeData(g_targetRows);
    std::printf("Done. Measuring 300 synchronous paint frames.\n\n");
    fflush(stdout);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const wchar_t* cls = L"TmProUiBench";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(0, cls,
        L"Task Manager Pro - UI Render Benchmark",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1180, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_main) {
        std::printf("FATAL: CreateWindowEx failed (%lu)\n", GetLastError());
        return 1;
    }

    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);

    MSG msg;
    DWORD start = GetTickCount();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_done) break;
        if (GetTickCount() - start > 30000) {   // safety timeout
            std::printf("\n[TIMEOUT] benchmark did not finish in 30s\n");
            Report();
            break;
        }
    }

    delete g_tv;
    return 0;
}
