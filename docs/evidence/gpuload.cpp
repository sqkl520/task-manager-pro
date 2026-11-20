// _probe/gpuload.cpp - 造 GPU 负载，用于验证 gpuinfo 使用率与 nvidia-smi 一致
// 用 D3D11 计算着色器做持续的算术循环，把 3D 引擎压到可观测水平。
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char* kCS = R"(
cbuffer Params : register(b0) { uint iters; uint pad[3]; };
RWStructuredBuffer<float> buf : register(u0);
[numthreads(64,1,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float x = buf[tid.x];
    for (uint i = 0; i < iters; ++i) {
        x = sin(x) * cos(x) + sqrt(abs(x) + 1.0);
        x = frac(x * 1.0001);
    }
    buf[tid.x] = x;
}
)";

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int iters = argc > 2 ? atoi(argv[2]) : 20000;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr); return 1; }
    printf("device ok, FL=0x%04X\n", (unsigned)fl);

    ID3DBlob* cs = nullptr;
    ID3DBlob* err = nullptr;
    hr = D3DCompile(kCS, strlen(kCS), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) {
        printf("D3DCompile failed 0x%08lX: %s\n", (unsigned long)hr, err ? (char*)err->GetBufferPointer() : "");
        return 1;
    }
    ID3D11ComputeShader* shader = nullptr;
    dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &shader);
    cs->Release();

    const UINT N = 1 << 20;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = N * sizeof(float);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(float);
    ID3D11Buffer* buf = nullptr;
    dev->CreateBuffer(&bd, nullptr, &buf);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = N;
    ID3D11UnorderedAccessView* uav = nullptr;
    dev->CreateUnorderedAccessView(buf, &uavd, &uav);

    struct Params { UINT iters; UINT pad[3]; } params{ (UINT)iters, {0,0,0} };
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Params);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr;
    dev->CreateBuffer(&cbd, nullptr, &cb);

    printf("running %d s, %d iters/dispatch\n", seconds, iters);
    DWORD start = GetTickCount();
    long dispatches = 0;
    while ((int)((GetTickCount() - start) / 1000) < seconds) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &params, sizeof(params));
            ctx->Unmap(cb, 0);
        }
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->Dispatch(N / 64, 1, 1);
        ++dispatches;
        ctx->Flush();
    }
    printf("done: %ld dispatches\n", dispatches);
    uav->Release(); buf->Release(); cb->Release(); shader->Release();
    ctx->Release(); dev->Release();
    return 0;
}
