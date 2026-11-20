// gpuinfo_probe6.cpp - 第六轮：DirectX 版本探测的最低成本方案
//
// 背景：probe3 实测 D3D11CreateDevice 每次 592~955 ms，远超 5ms 预算。
// 本探针比较几种方案，找出能在采集路径外完成（或更快完成）的做法：
//   A) D3D11CreateDevice(默认 6 个 FL)
//   B) D3D11CreateDevice(只请求 FL 12_1, SINGLETHREADED)
//   C) D3D11CreateDevice(指定真实 LUID 对应的 IDXGIAdapter)
//   D) IDXGIAdapter::CheckInterfaceSupport(ID3D10Device / ID3D11Device)  成本 + 返回值
//   E) D3D12CreateDevice(FL 11_0) 成本
//   F) 注册表里是否有任何 DirectX 版本线索
//   G) 设备创建在后台线程上的耗时（确认与线程无关）
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")

static double NowMs() {
    static LARGE_INTEGER f{}; static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
struct ThreadArg { double ms; HRESULT hr; D3D_FEATURE_LEVEL fl; };
static DWORD WINAPI ThreadProc(LPVOID p) {
    auto* a = (ThreadArg*)p;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                   D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    double t0 = NowMs();
    a->hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, _countof(levels),
                              D3D11_SDK_VERSION, &dev, &a->fl, &ctx);
    a->ms = NowMs() - t0;
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return 0;
}

int main() {
    printf("=== probe6: DirectX 版本最低成本方案 ===\n\n");

    // ---- A) 默认 6 FL ----
    {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                       D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got{};
        ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
        double t0 = NowMs();
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, _countof(levels),
                                      D3D11_SDK_VERSION, &d, &got, &c);
        printf("A) 默认(6 FL): hr=0x%08lX FL=0x%04X %.1f ms\n", (unsigned long)hr, (unsigned)got, NowMs() - t0);
        if (c) c->Release(); if (d) d->Release();
    }
    // ---- B) 只请求 12_1 + SINGLETHREADED ----
    {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1 };
        D3D_FEATURE_LEVEL got{};
        ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
        double t0 = NowMs();
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                                      levels, 1, D3D11_SDK_VERSION, &d, &got, &c);
        printf("B) 仅 FL12_1+SINGLETHREADED: hr=0x%08lX FL=0x%04X %.1f ms\n",
               (unsigned long)hr, (unsigned)got, NowMs() - t0);
        if (c) c->Release(); if (d) d->Release();
    }
    // ---- C) 指定真实 LUID 的 IDXGIAdapter ----
    {
        IDXGIFactory1* fac = nullptr;
        CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
        IDXGIAdapter1* match = nullptr;
        if (fac) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* a = nullptr;
                if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC d{};
                if (a && SUCCEEDED(a->GetDesc(&d)) && d.VendorId == 0x10DE) { match = a; break; }
                if (a) a->Release();
            }
        }
        if (match) {
            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                           D3D_FEATURE_LEVEL_11_0 };
            D3D_FEATURE_LEVEL got{};
            ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
            double t0 = NowMs();
            HRESULT hr = D3D11CreateDevice(match, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, _countof(levels),
                                          D3D11_SDK_VERSION, &d, &got, &c);
            printf("C) 指定 adapter(LUID 匹配): hr=0x%08lX FL=0x%04X %.1f ms\n",
                   (unsigned long)hr, (unsigned)got, NowMs() - t0);
            if (c) c->Release(); if (d) d->Release();

            // ---- D) CheckInterfaceSupport ----
            for (int k = 0; k < 2; ++k) {
                GUID g = k == 0 ? __uuidof(ID3D10Device) : __uuidof(ID3D11Device);
                LARGE_INTEGER umd{};
                double t1 = NowMs();
                HRESULT h2 = match->CheckInterfaceSupport(g, &umd);
                printf("D) CheckInterfaceSupport(%s): hr=0x%08lX umd=0x%08llX %.3f ms\n",
                       k == 0 ? "ID3D10Device" : "ID3D11Device", (unsigned long)h2,
                       (unsigned long long)umd.QuadPart, NowMs() - t1);
            }
            match->Release();
        } else printf("C) 没找到 NVIDIA adapter\n");
        if (fac) fac->Release();
    }
    // ---- E) D3D12CreateDevice ----
    {
        IDXGIFactory1* fac = nullptr;
        CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
        IDXGIAdapter1* match = nullptr;
        if (fac) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* a = nullptr;
                if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC d{};
                if (a && SUCCEEDED(a->GetDesc(&d)) && d.VendorId == 0x10DE) { match = a; break; }
                if (a) a->Release();
            }
        }
        for (int k = 0; k < 2; ++k) {
            D3D_FEATURE_LEVEL want = k == 0 ? D3D_FEATURE_LEVEL_12_1 : D3D_FEATURE_LEVEL_11_0;
            ID3D12Device* dev = nullptr;
            double t0 = NowMs();
            HRESULT hr = D3D12CreateDevice(match, want, __uuidof(ID3D12Device), (void**)&dev);
            printf("E) D3D12CreateDevice(FL 0x%04X): hr=0x%08lX %.1f ms\n", (unsigned)want,
                   (unsigned long)hr, NowMs() - t0);
            if (dev) dev->Release();
        }
        if (match) match->Release();
        if (fac) fac->Release();
    }
    // ---- F) 注册表线索 ----
    printf("\nF) 注册表 DirectX 线索:\n");
    {
        const wchar_t* base = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000";
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            for (int i = 0; i < 400; ++i) {
                wchar_t name[512]; DWORD nsz = 512; BYTE data[1024]; DWORD dsz = sizeof(data); DWORD type = 0;
                if (RegEnumValueW(hk, i, name, &nsz, nullptr, &type, data, &dsz) != ERROR_SUCCESS) break;
                std::wstring nm = name;
                if (nm.find(L"DirectX") != std::wstring::npos || nm.find(L"D3D") != std::wstring::npos ||
                    nm.find(L"WDDM") != std::wstring::npos || nm.find(L"FeatureLevel") != std::wstring::npos)
                    printf("   %ls (type=%lu size=%lu)\n", name, type, dsz);
            }
            RegCloseKey(hk);
        }
        const wchar_t* dx = L"SOFTWARE\\Microsoft\\DirectX";
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, dx, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t ver[128] = L""; DWORD sz = sizeof(ver);
            if (RegQueryValueExW(hk, L"Version", nullptr, nullptr, (LPBYTE)ver, &sz) == ERROR_SUCCESS)
                printf("   HKLM\\SOFTWARE\\Microsoft\\DirectX\\Version = %ls\n", ver);
            for (int i = 0; i < 40; ++i) {
                wchar_t name[256]; DWORD nsz = 256;
                if (RegEnumKeyExW(hk, i, name, &nsz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                printf("   DirectX 子键: %ls\n", name);
            }
            RegCloseKey(hk);
        }
    }
    // ---- G) 后台线程成本 ----
    printf("\nG) 后台线程创建 D3D11 设备:\n");
    {
        ThreadArg a{};
        double t0 = NowMs();
        HANDLE h = CreateThread(nullptr, 0, ThreadProc, &a, 0, nullptr);
        WaitForSingleObject(h, 30000);
        CloseHandle(h);
        printf("   线程内 D3D11CreateDevice hr=0x%08lX FL=0x%04X 线程内耗时=%.1f ms 墙钟=%.1f ms\n",
               (unsigned long)a.hr, (unsigned)a.fl, a.ms, NowMs() - t0);
    }
    printf("\n=== done ===\n");
    return 0;
}
