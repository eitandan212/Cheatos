#include "snap.h"
#include "console.h"

#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <ctime>

#pragma comment(lib, "windowscodecs.lib")

extern "C" ID3D11Device*        Overlay_GetDevice();
extern "C" ID3D11DeviceContext* Overlay_GetContext();

namespace {

bool WritePng(const wchar_t* path, int w, int h, int stride, const uint8_t* bgra) {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;
    IWICStream* stream = nullptr; factory->CreateStream(&stream);
    stream->InitializeFromFilename(path, GENERIC_WRITE);
    IWICBitmapEncoder* enc = nullptr;
    factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* frame = nullptr;
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WritePixels(h, stride, stride * h, (BYTE*)bgra);
    frame->Commit();
    enc->Commit();
    frame->Release(); enc->Release(); stream->Release(); factory->Release();
    return true;
}

} // namespace

std::string Snap::Save() {
    ID3D11Device* dev = Overlay_GetDevice();
    ID3D11DeviceContext* ctx = Overlay_GetContext();
    if (!dev || !ctx) return {};
    ID3D11Texture2D* back = nullptr;
    // Device swap chain is not exposed directly; approximate by using the
    // current render-target attached to the output-merger.
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return {};
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    res->QueryInterface(IID_PPV_ARGS(&back));
    res->Release(); rtv->Release();
    if (!back) return {};

    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* stage = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &stage))) { back->Release(); return {}; }
    ctx->CopyResource(stage, back);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) { stage->Release(); back->Release(); return {}; }

    wchar_t nameW[MAX_PATH];
    char   nameA[MAX_PATH];
    std::time_t tt = std::time(nullptr);
    std::tm   tm{}; localtime_s(&tm, &tt);
    std::snprintf(nameA, MAX_PATH, "loko-%04d%02d%02d-%02d%02d%02d.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    MultiByteToWideChar(CP_UTF8, 0, nameA, -1, nameW, MAX_PATH);
    bool ok = WritePng(nameW, (int)d.Width, (int)d.Height, (int)m.RowPitch, (const uint8_t*)m.pData);
    ctx->Unmap(stage, 0);
    stage->Release(); back->Release();
    if (!ok) return {};
    Console::Success("snap -> %s", nameA);
    return std::string(nameA);
}
