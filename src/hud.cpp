#include "hud.h"
#include "state.h"
#include "aimbot.h"
#include "esp.h"
#include "misc.h"
#include "game.h"
#include "overlay.h"
#include "events.h"

#include "../imgui/imgui.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

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
    // Target selection compares against projected points, so the centre comes
    // from the client rectangle the projection uses.
    float cx = 0, cy = 0;
    Game::ScreenCenter(cx, cy);
    if (cx < 1.0f || cy < 1.0f) {
        cx = (float)Overlay::Width()  * 0.5f;
        cy = (float)Overlay::Height() * 0.5f;
    }
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

    // hit chance estimate: inverse of (distance / effective FOV) * smoothing
    float d = std::max(1.0f, tgt->distance);
    float fovBonus = std::min(1.0f, (float)Aimbot::Cfg().fov / std::max(1.0f, best));
    float smoothPenalty = 1.0f / std::max(1.0f, Aimbot::Cfg().smooth * 0.5f);
    float chance = std::clamp(fovBonus * smoothPenalty * (200.0f / d), 0.0f, 1.0f);
    char cb[48]; std::snprintf(cb, 48, "chance %d%%", (int)(chance * 100.0f));
    ImU32 cc = chance > 0.66f ? IM_COL32(0x55, 0xE3, 0x9A, 255)
             : chance > 0.33f ? IM_COL32(0xFF, 0xC2, 0x66, 255)
             :                  IM_COL32(0xE3, 0x4D, 0x5C, 255);
    dl->AddText(ImVec2(x + 10, y + 72), cc, cb);
    // chance bar
    dl->AddRectFilled(ImVec2(x + 110, y + 74), ImVec2(x + 110 + 90, y + 82),
                      IM_COL32(0, 0, 0, 180), 2.0f);
    dl->AddRectFilled(ImVec2(x + 110, y + 74), ImVec2(x + 110 + 90 * chance, y + 82),
                      cc, 2.0f);
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

void DrawEventLog(ImDrawList* dl) {
    auto& st = AppState::Cfg();
    if (!st.showIndicatorPanel || st.streamerMode) return;
    const auto& ring = Events::Snapshot();
    if (ring.empty()) return;

    double now = NowSec();
    const float panelW = 280.0f;
    const float lineH  = 16.0f;
    int shown = 0;
    float yStart = 80.0f;
    float x = (float)Overlay::Width() - panelW - 14.0f;

    // Count visible (younger than 8 s)
    for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
        if (now - it->t > 8.0) break;
        if (++shown >= 6) break;
    }
    if (!shown) return;
    float panelH = shown * lineH + 12.0f;
    dl->AddRectFilled(ImVec2(x, yStart), ImVec2(x + panelW, yStart + panelH), kPanel, 6.0f);
    dl->AddRect      (ImVec2(x, yStart), ImVec2(x + panelW, yStart + panelH), kBorder, 6.0f, 0, 1.0f);
    int i = 0;
    for (auto it = ring.rbegin(); it != ring.rend() && i < shown; ++it, ++i) {
        float age = (float)(now - it->t);
        if (age > 8.0f) break;
        float fade = 1.0f - (age / 8.0f);
        ImU32 tint = kAccent;
        switch (it->kind) {
            case Events::Kind::Aim:     tint = kAccent; break;
            case Events::Kind::Trigger: tint = kAccH;   break;
            case Events::Kind::Teleport:tint = IM_COL32(0xFF,0xC2,0x66,0xFF); break;
            case Events::Kind::Warn:    tint = kBad;    break;
            case Events::Kind::Config:  tint = kTextLo; break;
            default: break;
        }
        ImU32 col = (ImU32)((((uint32_t)(fade * 255.0f)) << 24) | (tint & 0x00FFFFFF));
        dl->AddText(ImVec2(x + 10, yStart + 6 + i * lineH), col, it->text.c_str());
    }
}

void DrawToolbar(ImDrawList* dl) {
    auto& st = AppState::Cfg();
    if (st.streamerMode) return;
    struct Btn { const char* label; bool active; };
    Btn btns[] = {
        { "AIM",   Aimbot::Cfg().mode != Aimbot::Mode::Off },
        { "TRG",   Aimbot::TrigCfg().enabled               },
        { "ESP",   ESP::Cfg().enabled                      },
        { "RDR",   ESP::Cfg().radar                        },
        { "3P",    Misc::Cfg().thirdPerson                 },
        { "BHOP",  Misc::Cfg().bhop                        },
    };
    const int n = (int)(sizeof(btns)/sizeof(btns[0]));
    const float w = 46.0f, h = 24.0f, gap = 6.0f;
    float total = n * w + (n - 1) * gap + 20.0f;
    float x = (float)Overlay::Width() * 0.5f - total * 0.5f;
    float y = 12.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + total, y + h + 8.0f), kPanel, 8.0f);
    dl->AddRect      (ImVec2(x, y), ImVec2(x + total, y + h + 8.0f), kBorder, 8.0f, 0, 1.0f);
    for (int i = 0; i < n; ++i) {
        float bx = x + 10 + i * (w + gap);
        ImU32 bg = btns[i].active ? IM_COL32(32, 72, 70, 255) : IM_COL32(26, 31, 39, 255);
        ImU32 fg = btns[i].active ? kAccH : kTextLo;
        dl->AddRectFilled(ImVec2(bx, y + 4), ImVec2(bx + w, y + 4 + h), bg, 5.0f);
        if (btns[i].active)
            dl->AddRectFilled(ImVec2(bx, y + 4 + h - 2), ImVec2(bx + w, y + 4 + h), kAccent, 2.0f);
        ImVec2 sz = ImGui::CalcTextSize(btns[i].label);
        dl->AddText(ImVec2(bx + (w - sz.x) * 0.5f, y + 4 + (h - sz.y) * 0.5f), fg, btns[i].label);
    }
}

void Hud::Render() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    DrawIndicatorPanel(dl);
    DrawFps(dl);
    DrawTargetInfoCard(dl);
    DrawEventLog(dl);
    DrawToolbar(dl);
}
