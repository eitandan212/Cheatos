#include "aimbot.h"
#include "game.h"
#include "overlay.h"
#include "offsets.h"
#include "state.h"
#include "events.h"

#include "../imgui/imgui.h"

#include <windows.h>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace {

Aimbot::Settings g_cfg;
Aimbot::Trigger  g_trig;

uint32_t g_stickyId   = 0;
int      g_lockFrames = 0;
float    g_remX = 0.0f, g_remY = 0.0f;

// RCS state: watch LMB-held camera yaw/pitch drift and pull inverse
double g_lmbDownAt   = 0.0;
float  g_rcsYaw      = 0.0f;
float  g_rcsPitch    = 0.0f;
bool   g_rcsHavePrev = false;

// trigger state
double g_lastTrigFire = 0.0;
int    g_burstCount   = 0;

struct VelTrack { Vec3 pos; Vec3 vel; double t = 0.0; bool have = false; };
std::unordered_map<uint32_t, VelTrack> g_vel;

double NowSec() {
    using C = std::chrono::steady_clock;
    return std::chrono::duration<double>(C::now().time_since_epoch()).count();
}

Vec3 LeadPoint(const Player& p, const Vec3& base, float dist) {
    if (!g_cfg.predict || g_cfg.bulletSpeed < 1.0f) return base;
    auto& v = g_vel[p.entityId];
    double now = NowSec();
    if (!v.have) { v.pos = p.root; v.vel = Vec3(); v.t = now; v.have = true; return base; }
    double dt = now - v.t;
    if (dt < 0.001) return base;
    Vec3 inst((p.root.x - v.pos.x) / (float)dt,
              (p.root.y - v.pos.y) / (float)dt,
              (p.root.z - v.pos.z) / (float)dt);
    v.vel.x = v.vel.x * 0.7f + inst.x * 0.3f;
    v.vel.y = v.vel.y * 0.7f + inst.y * 0.3f;
    v.vel.z = v.vel.z * 0.7f + inst.z * 0.3f;
    v.pos = p.root; v.t = now;
    float lead = dist / g_cfg.bulletSpeed;
    return Vec3(base.x + v.vel.x * lead,
                base.y + v.vel.y * lead,
                base.z + v.vel.z * lead);
}

Vec3 AimOffsetForBone(const Player& p, Aimbot::Bone b) {
    float h;
    switch (b) {
        case Aimbot::Bone::Head:  h = offsets::skeleton::head_height;  break;
        case Aimbot::Bone::Torso: h = offsets::skeleton::torso_height; break;
        case Aimbot::Bone::Legs:  h = offsets::skeleton::leg_height;   break;
        default:                  h = offsets::skeleton::head_height;  break;
    }
    if (p.inVehicle) h += offsets::skeleton::vehicle_window_h - offsets::skeleton::head_height;
    return Vec3(p.root.x, p.root.y + h, p.root.z);
}

// Collect candidate aim points per cfg.multi
void CollectAimPoints(const Player& p, std::vector<Vec3>& out) {
    out.clear();
    // Rage multipoint: more points, pick closest to crosshair
    if (g_cfg.multi == Aimbot::Multi::Full && p.hasRig) {
        for (auto& j : p.rig.joints) out.push_back(j);
        return;
    }
    if (g_cfg.multi == Aimbot::Multi::HeadTorso) {
        out.push_back(AimOffsetForBone(p, Aimbot::Bone::Head));
        out.push_back(AimOffsetForBone(p, Aimbot::Bone::Torso));
        return;
    }
    if (g_cfg.bone == Aimbot::Bone::Nearest && p.hasRig) {
        for (auto& j : p.rig.joints) out.push_back(j);
        return;
    }
    out.push_back(AimOffsetForBone(p, g_cfg.bone));
}

bool PickBestPoint(const Player& p, float cx, float cy, Vec3& best, float& bestSd) {
    std::vector<Vec3> pts;
    CollectAimPoints(p, pts);
    bestSd = 1e9f;
    for (auto& v : pts) {
        float s[2];
        if (!Game::WorldToScreen(v, s)) continue;
        float sd = std::hypot(s[0] - cx, s[1] - cy);
        if (sd < bestSd) { bestSd = sd; best = v; }
    }
    return bestSd < 1e8f;
}

void SendMouse(float dx, float dy) {
    g_remX += dx; g_remY += dy;
    LONG ix = (LONG)g_remX; LONG iy = (LONG)g_remY;
    g_remX -= ix; g_remY -= iy;
    if (ix == 0 && iy == 0) return;
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dx = ix; in.mi.dy = iy; in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
    Game::ReportAimMove((float)ix, (float)iy);
}

void SendLeftClick() {
    INPUT down{}, up{};
    down.type = INPUT_MOUSE; down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    up.type   = INPUT_MOUSE; up.mi.dwFlags   = MOUSEEVENTF_LEFTUP;
    SendInput(1, &down, sizeof(down));
    SendInput(1, &up,   sizeof(up));
}

void TriggerStep(const std::vector<Player>& players, int localIdx, float cx, float cy) {
    if (!g_trig.enabled) { g_burstCount = 0; return; }
    bool keyDown = (GetAsyncKeyState(g_trig.keyVk) & 0x8000) != 0;
    if (!keyDown) { g_burstCount = 0; return; }
    if (g_trig.onlyWhenAiming && !(GetAsyncKeyState(g_cfg.keyVk) & 0x8000)) return;
    double now = NowSec();
    if ((now - g_lastTrigFire) * 1000.0 < (double)g_trig.delayMs) return;
    if (g_trig.burstMax > 0 && g_burstCount >= g_trig.burstMax) return;

    for (int i = 0; i < (int)players.size(); ++i) {
        if (i == localIdx) continue;
        const auto& p = players[i];
        if (!p.alive && g_cfg.ignoreDead) continue;
        if (p.distance > g_cfg.maxDistance) continue;
        float s[2]; if (!Game::WorldToScreen(p.head, s)) continue;
        float sd = std::hypot(s[0] - cx, s[1] - cy);
        if (sd > g_trig.fovPx) continue;

        if (g_trig.headOnly) {
            // only when the center is on the head bone neighborhood
            if (s[1] < cy - g_trig.fovPx * 2) continue;
        } else if (g_trig.bodyOnly) {
            float sb[2]; if (!Game::WorldToScreen(p.root, sb)) continue;
            if (sb[1] < cy) continue;
        }
        SendLeftClick();
        g_lastTrigFire = now;
        g_burstCount++;
        Events::Push(Events::Kind::Trigger, "fired on %s", p.name.empty() ? "target" : p.name.c_str());
        return;
    }
}

bool PassSafePoint(float sx, float sy) {
    float W = (float)Overlay::Width(), H = (float)Overlay::Height();
    float e = g_cfg.safePointEdge;
    return sx > e && sy > e && sx < W - e && sy < H - e;
}

} // namespace

Aimbot::Settings& Aimbot::Cfg()     { return g_cfg; }
Aimbot::Trigger&  Aimbot::TrigCfg() { return g_trig; }

void Aimbot::Update() {
    if (AppState::Cfg().panicActive) {
        g_stickyId = 0; g_lockFrames = 0;
        g_remX = g_remY = 0.0f;
        return;
    }
    std::vector<Player> players;
    Game::SnapshotPlayers(players);
    int localIdx = Game::LocalPlayerIndex(players);
    float cx = (float)Overlay::Width() * 0.5f;
    float cy = (float)Overlay::Height() * 0.5f;

    TriggerStep(players, localIdx, cx, cy);

    if (g_cfg.mode == Mode::Off) {
        g_stickyId = 0; g_lockFrames = 0;
        g_remX = g_remY = 0.0f;
        return;
    }
    if (!(GetAsyncKeyState(g_cfg.keyVk) & 0x8000)) {
        g_stickyId = 0; g_lockFrames = 0;
        g_remX = g_remY = 0.0f;
        return;
    }

    const Player* best = nullptr;
    float bestScore = 1e9f;
    Vec3  bestAim;
    float bestSd = 0;

    for (int i = 0; i < (int)players.size(); ++i) {
        if (i == localIdx) continue;
        const auto& p = players[i];
        if (!p.alive && g_cfg.ignoreDead) continue;
        if (p.distance > g_cfg.maxDistance) continue;
        if (AppState::IsFriend(p.name, p.clan)) continue;

        Bone chosenBone = g_cfg.bone;
        if (g_cfg.mode == Mode::Rage && p.health > g_cfg.baimHpAbove)
            chosenBone = Bone::Torso;

        Vec3 aim;
        float sd;
        if (g_cfg.mode == Mode::Rage) {
            if (!PickBestPoint(p, cx, cy, aim, sd)) continue;
        } else {
            aim = AimOffsetForBone(p, chosenBone);
            float s[2];
            if (!Game::WorldToScreen(aim, s)) continue;
            sd = std::hypot(s[0] - cx, s[1] - cy);
        }
        aim = LeadPoint(p, aim, p.distance);

        float s[2];
        if (!Game::WorldToScreen(aim, s)) continue;
        if (!PassSafePoint(s[0], s[1])) continue;
        sd = std::hypot(s[0] - cx, s[1] - cy);
        if (sd > g_cfg.fov) continue;

        if (g_cfg.sticky && g_stickyId && p.entityId == g_stickyId) {
            best = &p; bestAim = aim; bestSd = sd; bestScore = -1.0f; break;
        }
        float score = (g_cfg.priority == Priority::Crosshair) ? sd : p.distance;
        if (AppState::IsWatched(p.name, p.clan)) score -= 500.0f;
        if (score < bestScore) { bestScore = score; best = &p; bestAim = aim; bestSd = sd; }
    }

    if (!best) { g_stickyId = 0; g_lockFrames = 0; return; }

    if (g_stickyId == best->entityId) g_lockFrames++;
    else {
        g_lockFrames = 1;
        Events::Push(Events::Kind::Aim, "locked %s (%dm)",
                     best->name.empty() ? "unknown" : best->name.c_str(),
                     (int)best->distance);
    }
    g_stickyId = best->entityId;

    float s[2];
    if (!Game::WorldToScreen(bestAim, s)) return;

    if (g_cfg.memoryAim && Game::MemoryAim(bestAim)) {
        if (g_cfg.mode == Mode::Rage && g_cfg.autoFire && g_lockFrames >= g_cfg.lockDwell)
            SendLeftClick();
        if (g_cfg.drawAimLine) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddLine(ImVec2(cx, cy), ImVec2(s[0], s[1]), IM_COL32(124, 247, 240, 220), 1.4f);
        }
        return;
    }

    float dx = s[0] - cx;
    float dy = s[1] - cy;
    float scale = 1.0f / std::max(1.0f, g_cfg.smooth);
    float mx = dx * scale;
    float my = dy * scale;
    Game::ScaleAimMove(mx, my);
    SendMouse(mx, my);

    if (g_cfg.mode == Mode::Rage && g_cfg.autoFire && g_lockFrames >= g_cfg.lockDwell)
        SendLeftClick();

    if (g_cfg.drawAimLine) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddLine(ImVec2(cx, cy), ImVec2(s[0], s[1]), IM_COL32(124, 247, 240, 220), 1.4f);
    }
}

void Aimbot::ApplyRcs() {
    if (!g_cfg.rcs) { g_rcsHavePrev = false; return; }
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!lmb) { g_rcsHavePrev = false; return; }
    Cam c = Game::Camera();
    if (!c.valid) return;
    float yaw   = std::atan2(c.fwd.x, c.fwd.z);
    float pitch = std::asin(std::clamp(c.fwd.y, -1.0f, 1.0f));
    if (!g_rcsHavePrev) {
        g_rcsYaw = yaw; g_rcsPitch = pitch; g_rcsHavePrev = true;
        return;
    }
    float dYaw   = yaw - g_rcsYaw;
    float dPitch = pitch - g_rcsPitch;
    if (dYaw >  3.14159f) dYaw -= 6.2831853f;
    if (dYaw < -3.14159f) dYaw += 6.2831853f;
    // If pitch keeps rising (gun pulls up), apply inverse mouse down
    if (dPitch > 0.0005f) {
        float counter = -dPitch * 1800.0f * g_cfg.rcsStrength;
        SendMouse(0.0f, counter);
    }
    if (std::fabs(dYaw) > 0.0005f) {
        float counter = -dYaw * 1800.0f * g_cfg.rcsStrength * 0.5f;
        SendMouse(counter, 0.0f);
    }
    g_rcsYaw = yaw; g_rcsPitch = pitch;
}
