#include "state.h"
#include "events.h"

#include <algorithm>
#include <cstring>

namespace {
State g_state;

bool ieq_contains(const std::string& hay, const std::string& needle) {
    if (needle.empty() || hay.empty()) return false;
    auto low = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };
    return low(hay).find(low(needle)) != std::string::npos;
}

bool matches_list(const std::vector<NameEntry>& list,
                  const std::string& name, const std::string& clan) {
    for (auto& e : list) {
        std::string n(e.text);
        if (n.empty()) continue;
        if (ieq_contains(name, n) || ieq_contains(clan, n)) return true;
    }
    return false;
}
}

State& AppState::Cfg() { return g_state; }

void AppState::PollPanic() {
    // Panic toggles live even in Off mode via its own Toggle keybind.
    if (g_state.panic.mode == KeyMode::Off) {
        if (g_state.panicActive) { g_state.panicActive = false; Events::Push(Events::Kind::Warn, "panic disabled"); }
        return;
    }
    bool prev = g_state.panicActive;
    g_state.panicActive = g_state.panic.Active();
    if (g_state.panicActive != prev)
        Events::Push(Events::Kind::Warn, g_state.panicActive ? "PANIC ON" : "panic off");
}

bool AppState::IsWatched(const std::string& name, const std::string& clan) {
    return matches_list(g_state.watchlist, name, clan);
}
bool AppState::IsFriend(const std::string& name, const std::string& clan) {
    return matches_list(g_state.friends, name, clan);
}

bool AppState::LootPasses(const std::string& name, int rarity) {
    if (g_state.lootFilters.empty()) return true;
    bool anyWhite = false, whiteHit = false;
    for (auto& f : g_state.lootFilters) {
        if (!f.enabled) continue;
        bool hit = (rarity >= f.minRarity) &&
                   (f.needle[0] == 0 || ieq_contains(name, f.needle));
        if (f.whitelist) {
            anyWhite = true;
            if (hit) whiteHit = true;
        } else {
            if (hit) return false;   // blacklist match drops it
        }
    }
    return anyWhite ? whiteHit : true;
}
