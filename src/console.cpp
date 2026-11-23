#include "console.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
FILE*       g_file = nullptr;
std::mutex  g_lock;

void WriteLine(const char* level, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_file) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] [%s] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
    std::vfprintf(g_file, fmt, args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}
}

bool Console::Init() {
    if (g_file) return true;
    fopen_s(&g_file, "debug.log", "w");
    if (!g_file) return false;
    Log("console open");
    return true;
}

void Console::Shutdown() {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

void Console::Log(const char* fmt, ...)     { va_list a; va_start(a, fmt); WriteLine("INFO", fmt, a); va_end(a); }
void Console::Warn(const char* fmt, ...)    { va_list a; va_start(a, fmt); WriteLine("WARN", fmt, a); va_end(a); }
void Console::Error(const char* fmt, ...)   { va_list a; va_start(a, fmt); WriteLine("ERR ", fmt, a); va_end(a); }
void Console::Success(const char* fmt, ...) { va_list a; va_start(a, fmt); WriteLine("OK  ", fmt, a); va_end(a); }
