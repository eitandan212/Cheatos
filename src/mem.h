#pragma once

#include <cstdint>
#include <string>
#include <windows.h>

namespace Mem {

bool Init(DWORD pid);
void Shutdown();
bool IsConnected();
DWORD Pid();

bool ReadRaw (DWORD pid, uintptr_t addr, void* buf, size_t size);
bool WriteRaw(DWORD pid, uintptr_t addr, const void* buf, size_t size);

// Thin alias retained for callers sized to the old 0x1000 driver cap.
bool ReadChunked(DWORD pid, uintptr_t addr, void* buf, size_t size);

uintptr_t GetModuleBase(const wchar_t* name);

inline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFFFFFFULL;
}

template <typename T>
inline T ReadT(uintptr_t addr) {
    T v{};
    ReadRaw(Pid(), addr, &v, sizeof(T));
    return v;
}

inline uintptr_t ReadPtr(uintptr_t addr) {
    return ReadT<uintptr_t>(addr);
}

template <typename T>
inline bool Read(DWORD pid, uintptr_t addr, T& out) {
    return ReadRaw(pid, addr, &out, sizeof(out));
}

std::string ReadIl2CppString(uintptr_t str);
std::string ReadBoxedString (uintptr_t boxed);
std::string ReadNativeName  (uintptr_t addr, size_t max = 128);

} // namespace Mem
