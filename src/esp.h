#pragma once

#include <cstdint>

namespace ESP {

struct Settings {
    // master
    bool  enabled      = true;
    bool  showDead     = false;
    bool  showOffscreen= true;
    float maxDistance  = 400.0f;
    float boxScale     = 1.0f;

    // features
    bool  box          = true;
    bool  cornerBox    = true;
    bool  snapline     = false;
    bool  healthBar    = true;
    bool  nameTag      = true;
    bool  distanceTag  = true;
    bool  roleTag      = true;
    bool  heldItemTag  = true;
    bool  armorTag     = false;
    bool  clanTag      = false;
    bool  portrait     = true;
    bool  headDot      = true;
    bool  skeleton     = true;
    bool  chamsWire    = false;
    bool  crosshair    = true;
    bool  watermark    = true;

    // radar
    bool  radar        = true;
    float radarSize    = 220.0f;
    float radarRange   = 300.0f;
    bool  radarLoot    = false;

    // loot
    bool  lootEsp      = true;
    int   lootMinRarity= 0;
    float lootMaxDistance = 250.0f;

    // world
    bool  vehicleEsp   = true;
    bool  doorEsp      = false;

    // colors (ABGR as little-endian 0xAARRGGBB where stored)
    uint32_t colorEnemy   = 0xFF4CE0D9;  // klar cyan — enemies
    uint32_t colorAlly    = 0xFF55E39A;  // mint — allies
    uint32_t colorDead    = 0xFF6A7482;  // slate
    uint32_t colorOwnTeam = 0xFF7CF7F0;  // bright cyan
    uint32_t colorText    = 0xFFE6EAF0;  // off-white
    uint32_t colorShadow  = 0xFF0F1115;  // near-black
};

Settings& Cfg();
void Render();

} // namespace ESP
