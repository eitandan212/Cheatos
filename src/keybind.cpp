#include "keybind.h"
#include <windows.h>
#include <cstdio>

bool Keybind::Active() {
    switch (mode) {
        case KeyMode::Off:    return false;
        case KeyMode::Always: return true;
        case KeyMode::Hold:
            if (!vk) return false;
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        case KeyMode::Toggle: {
            if (!vk) return _toggleState;
            // rising edge flips state
            static int lastDownState[256] = {};
            int down = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
            if (down && !lastDownState[vk & 0xFF]) _toggleState = !_toggleState;
            lastDownState[vk & 0xFF] = down;
            return _toggleState;
        }
    }
    return false;
}

const char* KeyModeName(KeyMode m) {
    switch (m) {
        case KeyMode::Off:    return "Off";
        case KeyMode::Always: return "Always";
        case KeyMode::Hold:   return "Hold";
        case KeyMode::Toggle: return "Toggle";
    }
    return "?";
}

const char* VkPrettyName(int vk) {
    static char buf[32];
    if (vk == 0) return "<none>";
    if (vk == VK_LBUTTON)  return "MOUSE 1";
    if (vk == VK_RBUTTON)  return "MOUSE 2";
    if (vk == VK_MBUTTON)  return "MOUSE 3";
    if (vk == VK_XBUTTON1) return "MOUSE 4";
    if (vk == VK_XBUTTON2) return "MOUSE 5";
    if (vk == VK_SHIFT)    return "SHIFT";
    if (vk == VK_CONTROL)  return "CTRL";
    if (vk == VK_MENU)     return "ALT";
    if (vk == VK_SPACE)    return "SPACE";
    if (vk == VK_TAB)      return "TAB";
    if (vk == VK_INSERT)   return "INS";
    if (vk == VK_END)      return "END";
    if (vk == VK_HOME)     return "HOME";
    if (vk == VK_DELETE)   return "DEL";
    if (vk == VK_RETURN)   return "ENTER";
    if (vk >= 'A' && vk <= 'Z') { std::snprintf(buf, 32, "%c", vk); return buf; }
    if (vk >= '0' && vk <= '9') { std::snprintf(buf, 32, "%c", vk); return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { std::snprintf(buf, 32, "F%d", vk - VK_F1 + 1); return buf; }
    std::snprintf(buf, 32, "0x%02X", vk);
    return buf;
}
