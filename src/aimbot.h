#pragma once

#include <cstdint>

namespace Aimbot {

enum class Mode     { Off, Legit, Rage };
enum class Bone     { Head, Torso, Legs, Nearest };
enum class Multi    { Single, HeadTorso, Full };
enum class Priority { Crosshair, Distance };

struct Settings {
    Mode  mode          = Mode::Off;
    bool  drawFov       = true;
    bool  drawAimLine   = false;
    bool  sticky        = true;
    bool  ignoreDead    = true;
    bool  memoryAim     = false;
    bool  predict       = true;
    bool  rcs           = false;
    float rcsStrength   = 0.65f;
    int   keyVk         = 0x02;    // right mouse
    float fov           = 150.0f;
    float smooth        = 3.5f;
    float maxDistance   = 300.0f;
    float bulletSpeed   = 450.0f;
    Bone     bone       = Bone::Head;
    Multi    multi      = Multi::Single;
    Priority priority   = Priority::Crosshair;

    // conversion / safety
    float sensScale     = 1.0f;    // trim on top of the measured counts-per-degree
    float maxCounts     = 220.0f;  // hard cap on counts emitted in one frame
    bool  requireFocus  = true;    // never emit input while the game is background

    // rage-mode extras
    int   lockDwell     = 3;       // frames of stable target before fire
    int   baimHpAbove   = 40;      // body-aim if target HP above this
    float safePointEdge = 10.0f;   // skip aim within N px of screen edge
    bool  autoFire      = false;   // click when locked
    int   autoFireMs    = 90;      // minimum gap between synthetic clicks
};

struct Trigger {
    bool  enabled    = false;
    int   keyVk      = 0x05;      // mouse 4
    int   delayMs    = 55;
    int   burstMax   = 0;         // 0 = unlimited
    int   fovPx      = 14;
    bool  onlyWhenAiming = true;  // also require main aim key held
    bool  headOnly   = false;
    bool  bodyOnly   = false;
};

Settings& Cfg();
Trigger&  TrigCfg();

void Update();

// called by misc/rcs integrator each frame (no-op if off)
void ApplyRcs();

} // namespace Aimbot
