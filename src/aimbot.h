#pragma once

#include <cstdint>

namespace Aimbot {

enum class Bone   { Head, Torso, Legs, Nearest };
enum class Priority { Crosshair, Distance };

struct Settings {
    bool  enabled      = false;
    bool  drawFov      = true;
    bool  sticky       = true;
    bool  ignoreDead   = true;
    bool  memoryAim    = false;
    bool  predict      = true;
    bool  recoilComp   = false;
    int   keyVk        = 0x02;   // right mouse
    float fov          = 150.0f;
    float smooth       = 3.5f;
    float maxDistance  = 300.0f;
    float bulletSpeed  = 450.0f;
    Bone     bone      = Bone::Head;
    Priority priority  = Priority::Crosshair;
};

Settings& Cfg();
void Update();

} // namespace Aimbot
