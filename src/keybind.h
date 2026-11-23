#pragma once

#include <cstdint>

enum class KeyMode : uint8_t { Off, Always, Hold, Toggle };

struct Keybind {
    int     vk   = 0;
    KeyMode mode = KeyMode::Hold;
    bool    _toggleState = false;   // runtime, do not serialize meaning

    bool Active();                  // reads live key state
};

const char* KeyModeName(KeyMode m);
const char* VkPrettyName(int vk);
