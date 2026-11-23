#pragma once

#include "keybind.h"

#include <cstdint>
#include <string>
#include <vector>

enum class ThemePreset : uint8_t { Cyan, Rose, Amber, Purple, Mono };

struct NameEntry { char text[32]; };

struct LootFilter {
    char     needle[32] = "";
    int      minRarity  = 0;
    bool     whitelist  = true;
    bool     enabled    = true;
};

struct State {
    // Global panic — zeroes behaviour without touching user config.
    Keybind panic   { 0x50, KeyMode::Toggle };   // 'P'
    bool    panicActive = false;

    // Streamer mode — hide watermark + menu badges from captured output.
    bool    streamerMode = false;

    // Theme
    ThemePreset theme = ThemePreset::Cyan;
    float   menuAlpha = 1.0f;

    // Watchlist / Friends — names or clan tags compared case-insensitive.
    std::vector<NameEntry> watchlist;
    std::vector<NameEntry> friends;

    // Loot filter rows (OR'd).
    std::vector<LootFilter> lootFilters;

    // Hud toggles
    bool showIndicatorPanel = true;
    bool showFps            = true;
    bool showTargetInfoCard = true;

    // Menu feel
    bool    menuAnimate = true;
    float   menuAnimT   = 0.0f;   // 0..1 slide-in progress
};

namespace AppState {
State& Cfg();
void PollPanic();                         // call once per frame
bool IsWatched(const std::string& name, const std::string& clan);
bool IsFriend (const std::string& name, const std::string& clan);
bool LootPasses(const std::string& name, int rarity);
}
