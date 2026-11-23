#pragma once

#include <cstdint>

namespace Misc {

struct Settings {
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

    // info
    bool  showFps         = true;
    char  watermarkText[32] = "loko.gg";
};

Settings& Cfg();
void Update();   // called every frame after Game::Update()

} // namespace Misc
