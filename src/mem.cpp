#include "mem.h"
#include "console.h"
#include "offsets.h"

#include <tlhelp32.h>
#include <cstring>

namespace {
HANDLE g_proc = nullptr;
DWORD  g_pid  = 0;
}

bool Mem::Init(DWORD pid) {
    Shutdown();
    g_pid  = pid;
    g_proc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                         PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                         FALSE, pid);
    if (!g_proc) {
        Console::Error("Mem::Init: OpenProcess failed, GetLastError=%lu", GetLastError());
        return false;
    }
    uintptr_t base = GetModuleBase(L"GameAssembly.dll");
    uint16_t mz = 0;
    if (!base || !ReadRaw(g_pid, base, &mz, sizeof(mz)) || mz != 0x5A4D) {
        Console::Error("Mem::Init: handle probe failed, GetLastError=%lu", GetLastError());
        CloseHandle(g_proc);
        g_proc = nullptr;
        return false;
    }
    Console::Success("Mem::Init: pid=%lu GameAssembly=%p", pid, (void*)base);
    return IsConnected();
}

void Mem::Shutdown() {
    if (g_proc) { CloseHandle(g_proc); g_proc = nullptr; }
    g_pid = 0;
}

bool Mem::IsConnected() {
    return g_proc && WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT;
}

DWORD Mem::Pid() { return g_pid; }

bool Mem::ReadRaw(DWORD /*pid*/, uintptr_t addr, void* buf, size_t size) {
    if (!g_proc || !buf || !size || !IsValidPtr(addr)) return false;
    auto* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < size) {
        const size_t pageLeft = 0x1000 - ((addr + done) & 0xFFF);
        const size_t chunk    = (size - done) < pageLeft ? (size - done) : pageLeft;
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_proc, (LPCVOID)(addr + done), out + done, chunk, &got) || got != chunk) {
            std::memset(out + done, 0, size - done);
            return false;
        }
        done += chunk;
    }
    return true;
}

bool Mem::WriteRaw(DWORD /*pid*/, uintptr_t addr, const void* buf, size_t size) {
    if (!g_proc || !buf || !size || !IsValidPtr(addr)) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(g_proc, (LPVOID)addr, buf, size, &put) && put == size;
}

bool Mem::ReadChunked(DWORD pid, uintptr_t addr, void* buf, size_t size) {
    return ReadRaw(pid, addr, buf, size);
}

uintptr_t Mem::GetModuleBase(const wchar_t* name) {
    if (!g_pid || !name) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) {
                base = (uintptr_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

std::string Mem::ReadIl2CppString(uintptr_t str) {
    if (!IsValidPtr(str)) return {};
    int32_t len = ReadT<int32_t>(str + offsets::il2cpp_string_length);
    if (len <= 0 || len > 512) return {};
    wchar_t buf[514] = {};
    if (!ReadRaw(g_pid, str + offsets::il2cpp_string_chars, buf, (size_t)len * 2)) return {};
    buf[len] = 0;
    int nb = WideCharToMultiByte(CP_UTF8, 0, buf, len, nullptr, 0, nullptr, nullptr);
    if (nb <= 0) return {};
    std::string out(nb, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, len, out.data(), nb, nullptr, nullptr);
    return out;
}

std::string Mem::ReadBoxedString(uintptr_t boxed) {
    if (!IsValidPtr(boxed)) return {};
    uintptr_t inner = ReadPtr(boxed + 0x18);
    return ReadIl2CppString(inner);
}

std::string Mem::ReadNativeName(uintptr_t addr, size_t max) {
    if (!IsValidPtr(addr)) return {};
    if (max > 256) max = 256;
    char buf[257] = {};
    if (!ReadRaw(g_pid, addr, buf, max)) return {};
    buf[max] = 0;
    // scan to first non-printable
    for (size_t i = 0; i < max; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) { buf[i] = 0; break; }
        if (c < 0x20 || c > 0x7E) { buf[i] = 0; break; }
    }
    return std::string(buf);
}
