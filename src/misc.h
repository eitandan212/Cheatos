#pragma once

#include <cstdint>

#include "game.h"

namespace Misc {

struct SlotKey {
    int saveVk   = 0;
    int recallVk = 0;
};

struct Settings {
    // saved teleport slots — save = press saveVk, recall = press recallVk
    bool     slotHave[4]       = { false, false, false, false };
    float    slotPos[4][3]     = {};
    SlotKey  slotKeys[4]       = {};
    // movement
    bool  bhop            = false;       // auto space on LMB release / hold
    int   bhopKey         = 0x20;        // VK_SPACE
    bool  fastDuck        = false;
    int   fastDuckHz      = 14;
    bool  fakeDuck        = false;
    bool  airstuck        = false;       // hold current position in fly/air
    int   airstuckKey     = 0x58;        // VK 'X'

    // camera
    bool  thirdPerson     = false;
    float thirdDistance   = 2.6f;
    float thirdHeight     = 0.6f;
    bool  fovOverride     = false;
    float fovDeg          = 90.0f;

    // gun pins — live here so they persist with the profile and so the pin
    // ticker can re-apply them without the menu being open.
    Game::GunToggles gun;

    // Stop emitting synthetic input while another window has focus.
    bool  pauseUnfocused  = true;

    // info
    bool  showFps         = true;
    char  watermarkText[32] = "loko.gg";
};

Settings& Cfg();
void Update();       // called every frame after Game::Update()
void ReleaseAll();   // release every key this module is holding down

} // namespace Misc
