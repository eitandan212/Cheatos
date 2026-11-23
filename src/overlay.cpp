#include "overlay.h"
#include "console.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_win32.h"
#include "../imgui/imgui_impl_dx11.h"

#include <cstdlib>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

HWND  g_hwnd = nullptr;
HDC   g_memDc = nullptr;
HBITMAP g_dib  = nullptr;
void* g_dibBits = nullptr;
int   g_w = 0, g_h = 0;

ID3D11Device*            g_dev   = nullptr;
ID3D11DeviceContext*     g_ctx   = nullptr;
IDXGISwapChain*          g_swap  = nullptr;
ID3D11Texture2D*         g_back  = nullptr;
ID3D11Texture2D*         g_stage = nullptr;
ID3D11RenderTargetView*  g_rtv   = nullptr;

wchar_t g_cls[17] = {};
UINT_PTR g_timer = 0;

LARGE_INTEGER g_freq{};
LARGE_INTEGER g_last{};

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void RandomClass(wchar_t* out) {
    static bool seeded = false;
    if (!seeded) { srand((unsigned)GetTickCount()); seeded = true; }
    for (int i = 0; i < 16; ++i) out[i] = L'a' + (rand() % 26);
    out[16] = 0;
}

void DestroyDib() {
    if (g_memDc) { DeleteDC(g_memDc); g_memDc = nullptr; }
    if (g_dib)   { DeleteObject(g_dib); g_dib = nullptr; }
    g_dibBits = nullptr;
}

bool CreateDib(int w, int h) {
    DestroyDib();
    HDC screen = GetDC(nullptr);
    g_memDc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(g_memDc, &bi, DIB_RGB_COLORS, &g_dibBits, nullptr, 0);
    if (!g_dib) return false;
    SelectObject(g_memDc, g_dib);
    return true;
}

void DestroyRt() {
    if (g_rtv)  { g_rtv->Release();  g_rtv  = nullptr; }
    if (g_back) { g_back->Release(); g_back = nullptr; }
    if (g_stage){ g_stage->Release();g_stage= nullptr; }
}

bool CreateRtForSize(int w, int h) {
    DestroyRt();
    if (g_swap) {
        g_swap->ResizeBuffers(1, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    }
    if (g_swap && SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&g_back))) {
        g_dev->CreateRenderTargetView(g_back, nullptr, &g_rtv);
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &g_stage));
}

bool CreateDevice(int w, int h) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL out;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        0, &fl, 1, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &out, &g_ctx);
    if (FAILED(hr)) {
        Console::Error("D3D11CreateDeviceAndSwapChain failed hr=0x%08lX", hr);
        return false;
    }
    return CreateRtForSize(w, h);
}

}

bool Overlay::Init() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    RandomClass(g_cls);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = g_cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_w = sw; g_h = sh;

    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    g_hwnd = CreateWindowExW(ex, g_cls, L"", WS_POPUP,
                             0, 0, sw, sh, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;

    SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(g_hwnd, SW_SHOW);

    if (!CreateDib(sw, sh))    return false;
    if (!CreateDevice(sw, sh)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(g_hwnd)) return false;
    if (!ImGui_ImplDX11_Init(g_dev, g_ctx)) return false;

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    Console::Success("Overlay: %dx%d WARP device", sw, sh);
    return true;
}

void Overlay::Shutdown() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    DestroyRt();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx)  { g_ctx->Release();  g_ctx  = nullptr; }
    if (g_dev)  { g_dev->Release();  g_dev  = nullptr; }
    DestroyDib();
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    UnregisterClassW(g_cls, GetModuleHandleW(nullptr));
}

HWND Overlay::Hwnd()   { return g_hwnd; }
int  Overlay::Width()  { return g_w; }
int  Overlay::Height() { return g_h; }

bool Overlay::BeginFrame() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) return false;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    return true;
}

static void PaceTo(double targetHz) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart;
    double target = 1.0 / targetHz;
    if (elapsed < target) {
        double remaining = target - elapsed;
        if (remaining > 0.004) Sleep((DWORD)((remaining - 0.002) * 1000.0));
        do {
            QueryPerformanceCounter(&now);
            elapsed = (double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart;
        } while (elapsed < target);
    }
    g_last = now;
}

void Overlay::EndFrame() {
    if (!g_rtv || !g_back || !g_stage) return;
    const float clear[4] = { 0, 0, 0, 0 };
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->CopyResource(g_stage, g_back);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(g_ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &map))) {
        if (g_dibBits) {
            uint8_t* dst = (uint8_t*)g_dibBits;
            uint8_t* src = (uint8_t*)map.pData;
            const int rowBytes = g_w * 4;
            for (int y = 0; y < g_h; ++y) {
                memcpy(dst + y * rowBytes, src + y * map.RowPitch, rowBytes);
            }
        }
        g_ctx->Unmap(g_stage, 0);
    }
    POINT ptSrc{ 0, 0 };
    SIZE  sz  { g_w, g_h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, nullptr, nullptr, &sz, g_memDc, &ptSrc, 0, &bf, ULW_ALPHA);
    PaceTo(144.0);
}

void Overlay::MatchWindow(HWND game) {
    if (!game || !g_hwnd) return;
    // Track the CLIENT rectangle, not the window rectangle. World-to-screen
    // projects into client space, so an overlay sized to the window would be
    // offset by the border and title bar in windowed mode and every drawn
    // point, including the aim centre, would sit wrong by that much.
    RECT cr;
    if (!GetClientRect(game, &cr)) return;
    POINT tl{ cr.left, cr.top };
    if (!ClientToScreen(game, &tl)) return;
    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return;
    if (w != g_w || h != g_h) {
        g_w = w; g_h = h;
        CreateDib(w, h);
        CreateRtForSize(w, h);
    }
    static POINT s_last{ MAXLONG, MAXLONG };
    static int   s_lastW = 0, s_lastH = 0;
    if (tl.x != s_last.x || tl.y != s_last.y || w != s_lastW || h != s_lastH) {
        s_last = tl; s_lastW = w; s_lastH = h;
        SetWindowPos(g_hwnd, HWND_TOPMOST, tl.x, tl.y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void Overlay::SetClickThrough(bool through) {
    if (!g_hwnd) return;
    const LONG ex  = GetWindowLongW(g_hwnd, GWL_EXSTYLE);
    const LONG want = through ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (want == ex) return;                 // called twice a frame, only act on change
    SetWindowLongW(g_hwnd, GWL_EXSTYLE, want);
}

extern "C" ID3D11Device*        Overlay_GetDevice()  { return g_dev; }
extern "C" ID3D11DeviceContext* Overlay_GetContext() { return g_ctx; }

void Overlay::SetMenuAlpha(float a) {
    if (a < 0.1f) a = 0.1f;
    if (a > 1.0f) a = 1.0f;
    ImGui::GetStyle().Alpha = a;
}

struct Palette {
    int accentR, accentG, accentB;
    int accentHiR, accentHiG, accentHiB;
    int accentDimR, accentDimG, accentDimB;
};
static Palette g_pal = { 0x4C, 0xE0, 0xD9,  0x7C, 0xF7, 0xF0,  0x32, 0xAF, 0xAA };

void Overlay::ApplyThemePreset(int preset) {
    switch (preset) {
        case 0: g_pal = { 0x4C,0xE0,0xD9, 0x7C,0xF7,0xF0, 0x32,0xAF,0xAA }; break; // Cyan
        case 1: g_pal = { 0xE7,0x66,0x92, 0xFF,0x8C,0xB3, 0xB8,0x3B,0x66 }; break; // Rose
        case 2: g_pal = { 0xFF,0xC2,0x66, 0xFF,0xE0,0x94, 0xC6,0x8A,0x30 }; break; // Amber
        case 3: g_pal = { 0xB3,0x8C,0xFF, 0xD1,0xB8,0xFF, 0x7A,0x54,0xC6 }; break; // Purple
        case 4: g_pal = { 0xC9,0xCE,0xD6, 0xE6,0xEA,0xF0, 0x82,0x8C,0x9B }; break; // Mono
    }
    Overlay::ApplyThemeDarkRed();
}

void Overlay::ApplyThemeDarkRed() {
    // klar.gg: near-black glass base, cyan/mint accent, flat cards
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding     = 10.0f;
    s.ChildRounding      = 8.0f;
    s.FrameRounding      = 6.0f;
    s.GrabRounding       = 6.0f;
    s.ScrollbarRounding  = 6.0f;
    s.TabRounding        = 6.0f;
    s.PopupRounding      = 6.0f;
    s.WindowBorderSize   = 1.0f;
    s.ChildBorderSize    = 1.0f;
    s.FrameBorderSize    = 0.0f;
    s.PopupBorderSize    = 1.0f;
    s.WindowPadding      = ImVec2(16, 16);
    s.FramePadding       = ImVec2(10, 6);
    s.ItemSpacing        = ImVec2(12, 8);
    s.ItemInnerSpacing   = ImVec2(8, 6);
    s.IndentSpacing      = 18.0f;
    s.ScrollbarSize      = 12.0f;
    s.GrabMinSize        = 14.0f;
    s.WindowTitleAlign   = ImVec2(0.0f, 0.5f);

    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a){ return ImVec4(r/255.f, g/255.f, b/255.f, a); };

    const ImVec4 bg      = rgb(15, 17, 21, 0.97f);
    const ImVec4 panel   = rgb(20, 23, 29, 1.00f);
    const ImVec4 panelHi = rgb(26, 31, 39, 1.00f);
    const ImVec4 border  = rgb(42, 49, 61, 1.00f);
    const ImVec4 accent  = rgb(g_pal.accentR,    g_pal.accentG,    g_pal.accentB,    1.00f);
    const ImVec4 accentD = rgb(g_pal.accentDimR, g_pal.accentDimG, g_pal.accentDimB, 1.00f);
    const ImVec4 accentH = rgb(g_pal.accentHiR,  g_pal.accentHiG,  g_pal.accentHiB,  1.00f);
    const ImVec4 textHi  = rgb(230, 234, 240, 1.00f);
    const ImVec4 textLo  = rgb(130, 140, 155, 1.00f);

    c[ImGuiCol_WindowBg]           = bg;
    c[ImGuiCol_ChildBg]            = panel;
    c[ImGuiCol_PopupBg]            = rgb(16, 19, 24, 0.98f);
    c[ImGuiCol_Border]             = border;
    c[ImGuiCol_BorderShadow]       = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]            = panelHi;
    c[ImGuiCol_FrameBgHovered]     = rgb(34, 40, 50, 1.00f);
    c[ImGuiCol_FrameBgActive]      = rgb(40, 48, 60, 1.00f);
    c[ImGuiCol_TitleBg]            = rgb(15, 17, 21, 1.00f);
    c[ImGuiCol_TitleBgActive]      = rgb(18, 21, 27, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]   = rgb(15, 17, 21, 0.70f);
    c[ImGuiCol_MenuBarBg]          = panel;
    c[ImGuiCol_ScrollbarBg]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]      = rgb(50, 58, 72, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]= rgb(70, 80, 95, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]= accentD;
    c[ImGuiCol_CheckMark]          = accent;
    c[ImGuiCol_SliderGrab]         = accent;
    c[ImGuiCol_SliderGrabActive]   = accentH;
    c[ImGuiCol_Button]             = panelHi;
    c[ImGuiCol_ButtonHovered]      = rgb(34, 42, 54, 1.00f);
    c[ImGuiCol_ButtonActive]       = rgb(44, 55, 70, 1.00f);
    c[ImGuiCol_Header]             = rgb(30, 36, 46, 1.00f);
    c[ImGuiCol_HeaderHovered]      = rgb(38, 46, 58, 1.00f);
    c[ImGuiCol_HeaderActive]       = rgb(46, 58, 74, 1.00f);
    c[ImGuiCol_Separator]          = border;
    c[ImGuiCol_SeparatorHovered]   = accentD;
    c[ImGuiCol_SeparatorActive]    = accent;
    c[ImGuiCol_ResizeGrip]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]  = accentD;
    c[ImGuiCol_ResizeGripActive]   = accent;
    c[ImGuiCol_Tab]                = panelHi;
    c[ImGuiCol_TabHovered]         = rgb(34, 42, 54, 1.00f);
    c[ImGuiCol_TabActive]          = rgb(24, 46, 50, 1.00f);
    c[ImGuiCol_TabUnfocused]       = panel;
    c[ImGuiCol_TabUnfocusedActive] = rgb(22, 38, 42, 1.00f);
    c[ImGuiCol_PlotLines]          = accent;
    c[ImGuiCol_PlotLinesHovered]   = accentH;
    c[ImGuiCol_PlotHistogram]      = accent;
    c[ImGuiCol_PlotHistogramHovered]= accentH;
    c[ImGuiCol_TableHeaderBg]      = rgb(22, 26, 33, 1.00f);
    c[ImGuiCol_TableBorderStrong]  = border;
    c[ImGuiCol_TableBorderLight]   = rgb(30, 36, 46, 1.00f);
    c[ImGuiCol_TableRowBg]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]      = rgb(18, 21, 27, 0.50f);
    c[ImGuiCol_Text]               = textHi;
    c[ImGuiCol_TextDisabled]       = textLo;
    c[ImGuiCol_TextSelectedBg]     = rgb(50, 175, 170, 0.35f);
    c[ImGuiCol_DragDropTarget]     = accent;
    c[ImGuiCol_NavHighlight]       = accent;
    c[ImGuiCol_NavWindowingHighlight]= accentH;
    c[ImGuiCol_NavWindowingDimBg]  = rgb(0, 0, 0, 0.50f);
    c[ImGuiCol_ModalWindowDimBg]   = rgb(0, 0, 0, 0.60f);
}
