#include "portrait.h"
#include "portrait_images.h"
#include "console.h"

#include "../imgui/imgui.h"

#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <unordered_map>
#include <vector>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Overlay owns the device; expose a hook getter
extern "C" ID3D11Device*        Overlay_GetDevice();
extern "C" ID3D11DeviceContext* Overlay_GetContext();

namespace {

struct Entry {
    std::vector<uint8_t>       bytes;
    ID3D11ShaderResourceView*  srv = nullptr;
    int w = 0, h = 0;
    bool tried = false;
};

std::unordered_map<std::string, Entry> g_entries;
IWICImagingFactory* g_wic = nullptr;

bool Decode(Entry& e) {
    if (e.srv || e.tried) return e.srv != nullptr;
    e.tried = true;
    if (!g_wic || e.bytes.empty()) return false;
    IWICStream* stream = nullptr;
    if (FAILED(g_wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromMemory(e.bytes.data(), (DWORD)e.bytes.size()))) {
        stream->Release(); return false;
    }
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(g_wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
        stream->Release(); return false;
    }
    IWICBitmapFrameDecode* frame = nullptr;
    dec->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    IWICFormatConverter* conv = nullptr;
    g_wic->CreateFormatConverter(&conv);
    conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    std::vector<uint8_t> px(w * h * 4);
    conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data());
    conv->Release(); frame->Release(); dec->Release(); stream->Release();
    auto* dev = Overlay_GetDevice();
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = px.data();
    sd.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) return false;
    HRESULT hr = dev->CreateShaderResourceView(tex, nullptr, &e.srv);
    tex->Release();
    if (FAILED(hr)) return false;
    e.w = (int)w; e.h = (int)h;
    return true;
}

} // namespace

bool Portrait::Init() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&g_wic));
    if (FAILED(hr)) { Console::Warn("WIC factory init failed"); return false; }
    return true;
}

void Portrait::Shutdown() {
    for (auto& kv : g_entries) if (kv.second.srv) kv.second.srv->Release();
    g_entries.clear();
    if (g_wic) { g_wic->Release(); g_wic = nullptr; }
    CoUninitialize();
}

void Portrait::Register(const std::string& key, const uint8_t* bytes, size_t size) {
    if (!bytes || !size) return;
    Entry e;
    e.bytes.assign(bytes, bytes + size);
    g_entries[key] = std::move(e);
}

void Portrait::RegisterAll() {
    for (const auto& r : PortraitImages::All())
        Register(r.key, r.data, r.size);
}

void Portrait::Draw(const std::string& key, float x, float y, float w, float h) {
    auto it = g_entries.find(key);
    if (it == g_entries.end()) {
        it = g_entries.find("default");
        if (it == g_entries.end()) return;
    }
    if (!Decode(it->second) || !it->second.srv) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float src_aspect = (float)it->second.w / (float)it->second.h;
    float dst_aspect = w / h;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (src_aspect > dst_aspect) {
        float ratio = dst_aspect / src_aspect;
        float extra = (1.0f - ratio) * 0.5f;
        uv0.x = extra; uv1.x = 1.0f - extra;
    } else {
        float ratio = src_aspect / dst_aspect;
        float extra = (1.0f - ratio) * 0.5f;
        uv0.y = extra; uv1.y = 1.0f - extra;
    }
    dl->AddImageRounded((ImTextureID)it->second.srv,
                        ImVec2(x, y), ImVec2(x + w, y + h),
                        uv0, uv1, IM_COL32_WHITE, 4.0f);
}
