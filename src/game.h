#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <array>
#include <windows.h>

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};

struct Vec2 { float x = 0, y = 0; };

struct Cam {
    Vec3 pos;
    Vec3 fwd{0,0,1};
    Vec3 right{1,0,0};
    Vec3 up{0,1,0};
    float fov = 60.0f;
    bool  valid = false;
};

struct BoneRig {
    bool filled = false;
    std::array<Vec3, 16> joints{};
};

struct Player {
    uintptr_t   model = 0;
    uint32_t    entityId = 0;
    std::string name;
    std::string clan;
    std::string role;
    std::string heldItem;
    int32_t     health = 0;
    int32_t     maxHealth = 100;
    int32_t     armor = 0;
    Vec3        root;
    Vec3        head;
    BoneRig     rig;
    bool        hasRig = false;
    bool        crouch = false;
    bool        inVehicle = false;
    bool        isLocalPlayer = false;
    bool        alive = true;
    float       distance = 0.0f;
    Vec3        velocity;
    double      lastSeen = 0.0;
};

struct LootItem {
    uintptr_t   model = 0;
    std::string name;
    int         rarity = 0;      // 0..6
    Vec3        pos;
    float       distance = 0.0f;
};

struct WorldMark {
    enum Kind { Vehicle, Helicopter, Door };
    Kind        kind = Vehicle;
    uintptr_t   model = 0;
    std::string name;
    Vec3        pos;
    float       distance = 0.0f;
};

namespace Game {

// -------- lifecycle --------
DWORD FindProcess();               // returns pid or 0
bool  Init();                      // after Mem::Init; resolves module base + toolhelp window
bool  IsAlive();
void  Update();                    // called each frame
void  StartScanner();
void  StopScanner();

// -------- window --------
HWND  GameWindow();
int   ScreenW();
int   ScreenH();
void  RefreshWindow();
void  ScreenCenter(float& cx, float& cy);   // client-space center, projection-consistent
bool  IsForeground();                        // game window owns input focus

// -------- camera --------
Cam   Camera();

// -------- projection --------
bool  ProjectPoint(const Vec3& world, float out[2], float* outDepth = nullptr, bool cull = true);
bool  WorldToScreen(const Vec3& world, float out[2]);
bool  Project(const Vec3& world, float out[2], float* outDepth = nullptr);

// -------- snapshot --------
void  SnapshotPlayers(std::vector<Player>& out);
void  SnapshotLoot   (std::vector<LootItem>& out);
void  SnapshotMarks  (std::vector<WorldMark>& out);
int   LocalPlayerIndex(const std::vector<Player>& v);

// -------- teleport --------
enum class TpKind { None, ToEntity, PullEntity, ToPoint };
void  BeginTeleport(TpKind k, uint32_t entityId, const Vec3& point, bool fly = false);
void  CancelTeleport();
void  ApplyTeleport(bool steer);
void  ReinforceTeleport();
bool  TeleportRejected();        // server pulled the position back
int   TeleportRejectCount();

// -------- aim (memory path) --------
bool  MemoryAim(const Vec3& worldTarget);
void  ReinforceAim();

// -------- camera / player writes (klar misc feature set) --------
bool  WriteCameraFov(float deg);
bool  WriteThirdPerson(float backOffset, float upOffset);
void  DisableThirdPerson();
bool  WriteCrouch(bool crouched);
bool  LocalPlayerRoot(Vec3& out);

// -------- gun --------
struct GunToggles {
    bool noRecoil = false;
    bool noSpread = false;
    bool noSway   = false;
};
void  ApplyGun(const GunToggles& t);

// -------- gun pin ticking --------
void  TickGun();                 // re-applies active pins on the live model each frame
void  ReleaseGun();              // restores every pin still owned by a live model

// -------- sensitivity / aim conversion --------
// The regression learns mouse counts per degree of camera rotation from real
// moves. Until it has enough samples the estimate is derived from FOV and
// client height, which is a linearisation about screen centre, not a measurement.
bool  SensCalibrated();
void  SensCountsPerDeg(float& x, float& y);
int   SensSampleCount();
void  ResetSensCalibration();
bool  AimDeltaDegrees(const Vec3& target, float& dYawDeg, float& dPitchDeg);
void  AnglesToCounts(float dYawDeg, float dPitchDeg, float& mx, float& my);
void  ObserveAimTurn(const Vec3& fwd);
void  ReportAimMove(float dx, float dy);
void  ScaleAimMove(float& mx, float& my);    // legacy pixel path, calibration-free

// -------- diagnostics --------
struct Health {
    bool  rootsValid   = false;
    bool  cameraValid  = false;
    int   klassFound   = 0;
    int   klassTotal   = 0;
    int   klassGated   = 0;      // names given up on after repeated sweeps
    int   players      = 0;
    int   tpRejects    = 0;
};
Health Diagnostics();

// -------- misc --------
float Distance3D(const Vec3& a, const Vec3& b);

} // namespace Game
