#include "hud.h"
#include "state.h"
#include "aimbot.h"
#include "esp.h"
#include "misc.h"
#include "game.h"
#include "overlay.h"

#include "../imgui/imgui.h"
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

constexpr ImU32 kAccent = IM_COL32(0x4C, 0xE0, 0xD9, 0xFF);
constexpr ImU32 kAccH   = IM_COL32(0x7C, 0xF7, 0xF0, 0xFF);
constexpr ImU32 kPanel  = IM_COL32(0x14, 0x17, 0x1D, 220);
constexpr ImU32 kBorder = IM_COL32(0x2A, 0x31, 0x3D, 200);
constexpr ImU32 kTextHi = IM_COL32(0xE6, 0xEA, 0xF0, 0xFF);
constexpr ImU32 kTextLo = IM_COL32(0x82, 0x8C, 0x9B, 0xFF);
constexpr ImU32 kGood   = IM_COL32(0x55, 0xE3, 0x9A, 0xFF);
constexpr ImU32 kBad    = IM_COL32(0xE3, 0x4D, 0x5C, 0xFF);

double g_fpsLastT = 0.0;
int    g_fpsCount = 0;
float  g_fps      = 0.0f;

double NowSec() {
    using C = std::chrono::steady_clock;
    return std::chrono::duration<double>(C::now().time_since_epoch()).count();
}

struct Badge { const char* label; ImU32 color; };

void CollectBadges(std::vector<Badge>& out) {
    auto& a = Aimbot::Cfg();
    auto& t = Aimbot::TrigCfg();
    auto& m = Misc::Cfg();
    auto& s = AppState::Cfg();

    if (s.panicActive)              out.push_back({"PANIC",     kBad});
    if (a.mode == Aimbot::Mode::Rage) out.push_back({"RAGE",     kAccent});
    else if (a.mode == Aimbot::Mode::Legit) out.push_back({"LEGIT",kAccent});
    if (t.enabled)                  out.push_back({"TRIGGER",   kAccent});
    if (a.rcs)                      out.push_back({"RCS",       kAccH});
    if (m.bhop)                     out.push_back({"BHOP",      kAccH});
    if (m.fastDuck)                 out.push_back({"F-DUCK",    kAccH});
    if (m.fakeDuck)                 out.push_back({"K-DUCK",    kAccH});
    if (m.airstuck)                 out.push_back({"AIRSTUCK",  kAccH});
    if (m.thirdPerson)              out.push_back({"3P",        kAccH});
    if (m.fovOverride)              out.push_back({"FOV",       kAccH});
    if (ESP::Cfg().radar)           out.push_back({"RADAR",     kTextLo});
    if (ESP::Cfg().chamsWire)       out.push_back({"CHAMS",     kTextLo});
}

void DrawIndicatorPanel(ImDrawList* dl) {
    auto& s = AppState::Cfg();
    if (!s.showIndicatorPanel || s.streamerMode) return;
    std::vector<Badge> badges;
    CollectBadges(badges);
    if (badges.empty()) return;

    float x = 14.0f;
    float y = (float)Overlay::Height() - 20.0f;
    // bottom-left, grow upward
    for (int i = (int)badges.size() - 1; i >= 0; --i) {
        const char* txt = badges[i].label;
        ImVec2 sz = ImGui::CalcTextSize(txt);
        float bw = sz.x + 20.0f;
        float bh = 22.0f;
        dl->AddRectFilled(ImVec2(x, y - bh), ImVec2(x + bw, y), kPanel, 6.0f);
        dl->AddRect      (ImVec2(x, y - bh), ImVec2(x + bw, y), kBorder, 6.0f, 0, 1.0f);
        dl->AddRectFilled(ImVec2(x, y - bh), ImVec2(x + 3, y), badges[i].color, 2.0f);
        dl->AddText(ImVec2(x + 10, y - bh + 4), badges[i].color, txt);
        y -= bh + 6.0f;
    }
}

void DrawFps(ImDrawList* dl) {
    auto& s = AppState::Cfg();
    if (!s.showFps || s.streamerMode) return;
    char buf[32];
    std::snprintf(buf, 32, "%.0f fps", g_fps);
    ImVec2 sz = ImGui::CalcTextSize(buf);
    float x = (float)Overlay::Width() - sz.x - 20.0f;
    float y = (float)Overlay::Height() - 28.0f;
    dl->AddRectFilled(ImVec2(x - 8, y), ImVec2(x + sz.x + 8, y + 20), kPanel, 5.0f);
    dl->AddRect      (ImVec2(x - 8, y), ImVec2(x + sz.x + 8, y + 20), kBorder, 5.0f, 0, 1.0f);
    dl->AddText(ImVec2(x, y + 3), kAccH, buf);
}

void DrawTargetInfoCard(ImDrawList* dl) {
    auto& s = AppState::Cfg();
    if (!s.showTargetInfoCard) return;
    if (Aimbot::Cfg().mode == Aimbot::Mode::Off) return;

    std::vector<Player> players;
    Game::SnapshotPlayers(players);
    const Player* tgt = nullptr;
    // Pick the closest alive non-local on screen — same selection ESP
    // would highlight, but we do our own small pass so Aimbot state
    // doesn't need exposing.
    float cx = (float)Overlay::Width() * 0.5f;
    float cy = (float)Overlay::Height() * 0.5f;
    float best = 1e9f;
    for (auto& p : players) {
        if (p.isLocalPlayer || !p.alive) continue;
        float sp[2];
        if (!Game::WorldToScreen(p.head, sp)) continue;
        float d = std::hypot(sp[0] - cx, sp[1] - cy);
        if (d < best && d < Aimbot::Cfg().fov) { best = d; tgt = &p; }
    }
    if (!tgt) return;

    float w = 220.0f, h = 86.0f;
    float x = cx + 32.0f;
    float y = cy - h * 0.5f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kPanel, 8.0f);
    dl->AddRect      (ImVec2(x, y), ImVec2(x + w, y + h), kBorder, 8.0f, 0, 1.0f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 3, y + h), kAccent, 2.0f);
    char line[96];
    std::snprintf(line, 96, "%s", tgt->name.empty() ? "unknown" : tgt->name.c_str());
    dl->AddText(ImVec2(x + 10, y + 6), kTextHi, line);
    if (!tgt->clan.empty()) {
        std::snprintf(line, 96, "[%s]", tgt->clan.c_str());
        dl->AddText(ImVec2(x + 10 + (int)ImGui::CalcTextSize(tgt->name.c_str()).x + 8,
                           y + 6), kTextLo, line);
    }
    std::snprintf(line, 96, "hp  %d / %d", tgt->health, tgt->maxHealth);
    dl->AddText(ImVec2(x + 10, y + 24), kTextLo, line);
    std::snprintf(line, 96, "dist  %dm", (int)tgt->distance);
    dl->AddText(ImVec2(x + 10, y + 40), kTextLo, line);
    if (!tgt->role.empty())
        dl->AddText(ImVec2(x + 10, y + 56), kAccH, tgt->role.c_str());
    if (!tgt->heldItem.empty())
        dl->AddText(ImVec2(x + 10 + 70, y + 56), kTextLo, tgt->heldItem.c_str());
}

} // namespace

void Hud::TickFps() {
    double now = NowSec();
    if (g_fpsLastT == 0.0) g_fpsLastT = now;
    g_fpsCount++;
    if (now - g_fpsLastT >= 0.5) {
        g_fps = (float)(g_fpsCount / (now - g_fpsLastT));
        g_fpsLastT = now;
        g_fpsCount = 0;
    }
}

void Hud::Render() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    DrawIndicatorPanel(dl);
    DrawFps(dl);
    DrawTargetInfoCard(dl);
}
