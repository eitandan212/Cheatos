#include "clipboard.h"

#include <windows.h>
#include <cstring>

namespace {
const char kAlpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string Clip::Base64Encode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < size) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < size) n |= (uint32_t)data[i + 2];
        out.push_back(kAlpha[(n >> 18) & 0x3F]);
        out.push_back(kAlpha[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kAlpha[(n >> 6)  & 0x3F] : '=');
        out.push_back(i + 2 < size ? kAlpha[ n        & 0x3F] : '=');
    }
    return out;
}

bool Clip::Base64Decode(const std::string& b64, std::vector<uint8_t>& out) {
    int table[256];
    for (int i = 0; i < 256; ++i) table[i] = -1;
    for (int i = 0; i < 64; ++i) table[(unsigned char)kAlpha[i]] = i;
    out.clear();
    out.reserve(b64.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : b64) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = table[(unsigned char)c];
        if (v < 0) return false;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((buf >> bits) & 0xFF)); }
    }
    return true;
}

bool Clip::SetText(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!mem) { CloseClipboard(); return false; }
    char* dst = (char*)GlobalLock(mem);
    if (!dst) { GlobalFree(mem); CloseClipboard(); return false; }
    std::memcpy(dst, text.data(), text.size());
    dst[text.size()] = 0;
    GlobalUnlock(mem);
    SetClipboardData(CF_TEXT, mem);
    CloseClipboard();
    return true;
}

std::string Clip::GetText() {
    if (!OpenClipboard(nullptr)) return {};
    HANDLE h = GetClipboardData(CF_TEXT);
    std::string out;
    if (h) {
        const char* src = (const char*)GlobalLock(h);
        if (src) { out = src; GlobalUnlock(h); }
    }
    CloseClipboard();
    return out;
}
