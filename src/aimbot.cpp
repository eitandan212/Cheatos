#include "aimbot.h"
#include "game.h"
#include "overlay.h"
#include "offsets.h"

#include "../imgui/imgui.h"

#include <windows.h>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace {

Aimbot::Settings g_cfg;

uint32_t g_stickyId = 0;
float    g_remX = 0.0f, g_remY = 0.0f;

struct VelTrack {
    Vec3 pos;
    Vec3 vel;
    double t = 0.0;
    bool  have = false;
};
std::unordered_map<uint32_t, VelTrack> g_vel;

double NowSec() {
    using C = std::chrono::steady_clock;
    return std::chrono::duration<double>(C::now().time_since_epoch()).count();
}

Vec3 LeadPoint(const Player& p, float dist) {
    if (!g_cfg.predict || g_cfg.bulletSpeed < 1.0f) return p.head;
    auto& v = g_vel[p.entityId];
    double now = NowSec();
    if (!v.have) {
        v.pos = p.root; v.vel = Vec3(); v.t = now; v.have = true;
        return p.head;
    }
    double dt = now - v.t;
    if (dt < 0.001) return p.head;
    Vec3 inst = Vec3((p.root.x - v.pos.x) / (float)dt,
                     (p.root.y - v.pos.y) / (float)dt,
                     (p.root.z - v.pos.z) / (float)dt);
    v.vel.x = v.vel.x * 0.7f + inst.x * 0.3f;
    v.vel.y = v.vel.y * 0.7f + inst.y * 0.3f;
    v.vel.z = v.vel.z * 0.7f + inst.z * 0.3f;
    v.pos = p.root;
    v.t = now;
    float lead = dist / g_cfg.bulletSpeed;
    return Vec3(p.head.x + v.vel.x * lead,
                p.head.y + v.vel.y * lead,
                p.head.z + v.vel.z * lead);
}

Vec3 AimPoint(const Player& p) {
    float hOff = offsets::skeleton::head_height;
    switch (g_cfg.bone) {
        case Aimbot::Bone::Head:  hOff = offsets::skeleton::head_height; break;
        case Aimbot::Bone::Torso: hOff = offsets::skeleton::torso_height; break;
        case Aimbot::Bone::Legs:  hOff = offsets::skeleton::leg_height;   break;
        case Aimbot::Bone::Nearest: hOff = offsets::skeleton::head_height; break;
    }
    if (p.inVehicle) hOff += offsets::skeleton::vehicle_window_h - offsets::skeleton::head_height;
    Vec3 a = Vec3(p.root.x, p.root.y + hOff, p.root.z);
    if (g_cfg.bone == Aimbot::Bone::Nearest && p.hasRig) {
        // crosshair position
        float cx = (float)Overlay::Width() * 0.5f;
        float cy = (float)Overlay::Height() * 0.5f;
        float bestD = 1e9f;
        Vec3 best = p.head;
        for (auto& j : p.rig.joints) {
            float s[2];
            if (!Game::WorldToScreen(j, s)) continue;
            float d = std::hypot(s[0] - cx, s[1] - cy);
            if (d < bestD) { bestD = d; best = j; }
        }
        return best;
    }
    return a;
}

void SendMouse(float dx, float dy) {
    g_remX += dx;
    g_remY += dy;
    LONG ix = (LONG)g_remX;
    LONG iy = (LONG)g_remY;
    g_remX -= ix;
    g_remY -= iy;
    if (ix == 0 && iy == 0) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = ix;
    in.mi.dy = iy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
    Game::ReportAimMove((float)ix, (float)iy);
}

bool MemoryAim(const Vec3& /*target*/) {
    // placeholder for direct camera-write path; falls through to mouse
    return false;
}

} // namespace

Aimbot::Settings& Aimbot::Cfg() { return g_cfg; }

void Aimbot::Update() {
    if (!g_cfg.enabled) return;
    if (!(GetAsyncKeyState(g_cfg.keyVk) & 0x8000)) {
        g_stickyId = 0;
        g_remX = g_remY = 0.0f;
        return;
    }

    std::vector<Player> players;
    Game::SnapshotPlayers(players);
    int localIdx = Game::LocalPlayerIndex(players);

    float cx = (float)Overlay::Width() * 0.5f;
    float cy = (float)Overlay::Height() * 0.5f;

    const Player* best = nullptr;
    float bestScore = 1e9f;
    Vec3  bestAim;
    float bestDist = 0;

    for (int i = 0; i < (int)players.size(); ++i) {
        if (i == localIdx) continue;
        const auto& p = players[i];
        if (!p.alive && g_cfg.ignoreDead) continue;
        if (p.distance > g_cfg.maxDistance) continue;

        Vec3 aim = AimPoint(p);
        aim = LeadPoint(p, p.distance);

        float s[2];
        if (!Game::WorldToScreen(aim, s)) continue;
        float sd = std::hypot(s[0] - cx, s[1] - cy);
        if (sd > g_cfg.fov) continue;
        if (g_cfg.sticky && g_stickyId && p.entityId == g_stickyId) {
            bestScore = -1.0f;
            best = &p; bestAim = aim; bestDist = sd;
            break;
        }
        float score = (g_cfg.priority == Priority::Crosshair) ? sd : p.distance;
        if (score < bestScore) {
            bestScore = score;
            best = &p; bestAim = aim; bestDist = sd;
        }
    }

    if (!best) { g_stickyId = 0; return; }
    g_stickyId = best->entityId;

    float s[2];
    if (!Game::WorldToScreen(bestAim, s)) return;

    if (g_cfg.memoryAim && MemoryAim(bestAim)) return;

    float dx = s[0] - cx;
    float dy = s[1] - cy;
    float scale = 1.0f / std::max(1.0f, g_cfg.smooth);
    float mx = dx * scale;
    float my = dy * scale;
    Game::ScaleAimMove(mx, my);
    SendMouse(mx, my);
}
