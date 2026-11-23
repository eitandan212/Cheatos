#include "events.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::vector<Events::Entry> g_ring;
std::mutex                 g_lock;
constexpr size_t kCap = 48;

double NowSec() {
    using C = std::chrono::steady_clock;
    static auto t0 = C::now();
    return std::chrono::duration<double>(C::now() - t0).count();
}
}

void Events::Push(Kind k, const char* fmt, ...) {
    char buf[256];
    va_list a; va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> g(g_lock);
    g_ring.push_back(Entry{ k, std::string(buf), NowSec() });
    if (g_ring.size() > kCap) g_ring.erase(g_ring.begin(), g_ring.begin() + (g_ring.size() - kCap));
}

const std::vector<Events::Entry>& Events::Snapshot() { return g_ring; }

void Events::Clear() { std::lock_guard<std::mutex> g(g_lock); g_ring.clear(); }
