// Klar-style misc. Honest skips vs. klar: no clantag spoof (server-auth),
// no name stealer, no chat spammer, no hitsound (no outgoing-hit event).
#include "misc.h"
#include "game.h"
#include "events.h"

#include <windows.h>
#include <algorithm>
#include <chrono>

namespace {

Misc::Settings g_cfg;

Vec3   g_airstuckPos;
bool   g_airstuckHeld = false;

double NowSec() {
    using C = std::chrono::steady_clock;
    return std::chrono::duration<double>(C::now().time_since_epoch()).count();
}

void SendKey(int vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

double g_bhopNextUp   = 0.0;
bool   g_bhopKeyDown  = false;
double g_lastDuckEdge = 0.0;
bool   g_duckState    = false;

void StepBhop() {
    if (!g_cfg.bhop) {
        if (g_bhopKeyDown) { SendKey(g_cfg.bhopKey, false); g_bhopKeyDown = false; }
        return;
    }
    bool held = (GetAsyncKeyState(g_cfg.bhopKey) & 0x8000) != 0;
    if (!held) {
        if (g_bhopKeyDown) { SendKey(g_cfg.bhopKey, false); g_bhopKeyDown = false; }
        return;
    }
    // Simple cycle: key down ~40ms, key up ~40ms. Engine translates the
    // up-edge into a fresh jump on landing frame.
    double now = NowSec();
    if (g_bhopKeyDown) {
        if (now >= g_bhopNextUp) {
            SendKey(g_cfg.bhopKey, false);
            g_bhopKeyDown = false;
            g_bhopNextUp = now + 0.040;
        }
    } else {
        if (now >= g_bhopNextUp) {
            SendKey(g_cfg.bhopKey, true);
            g_bhopKeyDown = true;
            g_bhopNextUp = now + 0.040;
        }
    }
}

void StepFastDuck() {
    if (!g_cfg.fastDuck) {
        if (g_duckState) { SendKey(VK_LCONTROL, false); g_duckState = false; }
        return;
    }
    double period = 1.0 / (double)std::max(1, g_cfg.fastDuckHz);
    double now = NowSec();
    if (now - g_lastDuckEdge < period * 0.5) return;
    g_lastDuckEdge = now;
    g_duckState = !g_duckState;
    SendKey(VK_LCONTROL, g_duckState);
}

void StepFakeDuck() {
    if (!g_cfg.fakeDuck) return;
    // Toggle the crouch primitive bit each frame — 50% duty via alternation
    static bool flip = false;
    flip = !flip;
    Game::WriteCrouch(flip);
}

void StepAirstuck() {
    if (!g_cfg.airstuck) { g_airstuckHeld = false; return; }
    bool held = (GetAsyncKeyState(g_cfg.airstuckKey) & 0x8000) != 0;
    if (!held) { g_airstuckHeld = false; return; }
    if (!g_airstuckHeld) {
        if (!Game::LocalPlayerRoot(g_airstuckPos)) return;
        g_airstuckHeld = true;
    }
    Game::BeginTeleport(Game::TpKind::ToPoint, 0, g_airstuckPos, false);
}

void StepThirdPerson() {
    if (!g_cfg.thirdPerson) return;
    Game::WriteThirdPerson(g_cfg.thirdDistance, g_cfg.thirdHeight);
}

void StepFov() {
    if (!g_cfg.fovOverride) return;
    Game::WriteCameraFov(g_cfg.fovDeg);
}

int g_lastSlotState[4][2] = {}; // {save, recall}

void StepSavedSlots() {
    for (int i = 0; i < 4; ++i) {
        int saveVk   = g_cfg.slotKeys[i].saveVk;
        int recallVk = g_cfg.slotKeys[i].recallVk;
        int saveNow   = saveVk   && (GetAsyncKeyState(saveVk)   & 0x8000) ? 1 : 0;
        int recallNow = recallVk && (GetAsyncKeyState(recallVk) & 0x8000) ? 1 : 0;
        if (saveNow && !g_lastSlotState[i][0]) {
            Vec3 pos;
            if (Game::LocalPlayerRoot(pos)) {
                g_cfg.slotHave[i] = true;
                g_cfg.slotPos[i][0] = pos.x;
                g_cfg.slotPos[i][1] = pos.y;
                g_cfg.slotPos[i][2] = pos.z;
                Events::Push(Events::Kind::Teleport, "slot %d saved", i + 1);
            }
        }
        if (recallNow && !g_lastSlotState[i][1] && g_cfg.slotHave[i]) {
            Vec3 pt(g_cfg.slotPos[i][0], g_cfg.slotPos[i][1], g_cfg.slotPos[i][2]);
            Game::BeginTeleport(Game::TpKind::ToPoint, 0, pt, false);
            Events::Push(Events::Kind::Teleport, "slot %d recall", i + 1);
        }
        g_lastSlotState[i][0] = saveNow;
        g_lastSlotState[i][1] = recallNow;
    }
}

} // namespace

Misc::Settings& Misc::Cfg() { return g_cfg; }

void Misc::ReleaseAll() {
    if (g_bhopKeyDown) { SendKey(g_cfg.bhopKey, false); g_bhopKeyDown = false; }
    if (g_duckState)   { SendKey(VK_LCONTROL,   false); g_duckState   = false; }
    g_airstuckHeld = false;
}

void Misc::Update() {
    // Memory-side work is safe with the game in the background; synthetic key
    // events are not, they would land in whatever window has focus.
    const bool focused = Game::IsForeground();
    if (g_cfg.pauseUnfocused && !focused) {
        ReleaseAll();
        Game::TickGun();
        StepThirdPerson();
        StepFov();
        return;
    }

    StepBhop();
    StepFastDuck();
    StepFakeDuck();
    StepAirstuck();
    StepThirdPerson();
    StepFov();
    StepSavedSlots();
    Game::TickGun();
}
