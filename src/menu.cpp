#include "menu.h"
#include "esp.h"
#include "aimbot.h"
#include "game.h"
#include "overlay.h"
#include "console.h"
#include "misc.h"

#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

bool g_open = false;
int  g_section = 0;

struct SavedBlob {
    uint32_t magic = 0x4B4F4C52; // 'RLOK'
    uint32_t version = 3;
    ESP::Settings     esp;
    Aimbot::Settings  aim;
    Aimbot::Trigger   trig;
    Misc::Settings    misc;
};

int  g_activeProfile = 0;
char g_profileNames[3][16] = { "A", "B", "C" };

const char* VkName(int vk) {
    static char buf[32];
    if (vk == 0) return "<none>";
    if (vk == VK_LBUTTON)  return "MOUSE 1";
    if (vk == VK_RBUTTON)  return "MOUSE 2";
    if (vk == VK_MBUTTON)  return "MOUSE 3";
    if (vk == VK_XBUTTON1) return "MOUSE 4";
    if (vk == VK_XBUTTON2) return "MOUSE 5";
    if (vk == VK_SHIFT)    return "SHIFT";
    if (vk == VK_CONTROL)  return "CTRL";
    if (vk == VK_MENU)     return "ALT";
    if (vk == VK_SPACE)    return "SPACE";
    if (vk == VK_TAB)      return "TAB";
    if (vk == VK_INSERT)   return "INS";
    if (vk == VK_END)      return "END";
    if (vk >= 'A' && vk <= 'Z') { std::snprintf(buf, 32, "%c", vk); return buf; }
    if (vk >= '0' && vk <= '9') { std::snprintf(buf, 32, "%c", vk); return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { std::snprintf(buf, 32, "F%d", vk - VK_F1 + 1); return buf; }
    std::snprintf(buf, 32, "0x%02X", vk);
    return buf;
}

// Simple key-capture widget. While `*pending` is true, the next key hit
// becomes `vk`. Returns true on a change.
bool BindButton(const char* label, int* vk, bool* pending) {
    char btn[48];
    std::snprintf(btn, 48, "%s [%s]", *pending ? "press..." : label, VkName(*vk));
    bool changed = false;
    if (ImGui::Button(btn, ImVec2(-1, 0))) *pending = true;
    if (*pending) {
        for (int k = 1; k < 255; ++k) {
            if (k == VK_ESCAPE) { if (GetAsyncKeyState(k) & 1) { *pending = false; break; } continue; }
            if (GetAsyncKeyState(k) & 1) {
                *vk = k;
                *pending = false;
                changed = true;
                break;
            }
        }
    }
    return changed;
}

// ------------------------------------------------------------------
// Klar palette + helpers
// ------------------------------------------------------------------
constexpr ImU32 kAccent    = IM_COL32(0x4C, 0xE0, 0xD9, 0xFF);
constexpr ImU32 kAccentDim = IM_COL32(0x32, 0xAF, 0xAA, 0xFF);
constexpr ImU32 kAccentHi  = IM_COL32(0x7C, 0xF7, 0xF0, 0xFF);
constexpr ImU32 kPanel     = IM_COL32(0x14, 0x17, 0x1D, 0xFF);
constexpr ImU32 kPanelHi   = IM_COL32(0x1A, 0x1F, 0x27, 0xFF);
constexpr ImU32 kBorder    = IM_COL32(0x2A, 0x31, 0x3D, 0xFF);
constexpr ImU32 kTextHi    = IM_COL32(0xE6, 0xEA, 0xF0, 0xFF);
constexpr ImU32 kTextLo    = IM_COL32(0x82, 0x8C, 0x9B, 0xFF);

uint32_t ColorFromImVec(ImVec4 c) {
    auto cl = [](float v) { return (uint32_t)(v * 255.0f) & 0xFF; };
    return (cl(c.w) << 24) | (cl(c.z) << 16) | (cl(c.y) << 8) | cl(c.x);
}
ImVec4 ImVecFromColor(uint32_t c) {
    float a = ((c >> 24) & 0xFF) / 255.0f;
    float b = ((c >> 16) & 0xFF) / 255.0f;
    float g = ((c >> 8)  & 0xFF) / 255.0f;
    float r = ( c        & 0xFF) / 255.0f;
    return ImVec4(r, g, b, a);
}

// ------------------------------------------------------------------
// Primitives — card header, rail button, section title
// ------------------------------------------------------------------
void SectionTitle(const char* big, const char* sub = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 3, p.y + 20), kAccent, 2.0f);
    ImGui::Dummy(ImVec2(10, 0));
    ImGui::SameLine();
    ImGui::PushFont(ImGui::GetFont());
    ImGui::TextUnformatted(big);
    ImGui::PopFont();
    if (sub) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextLo));
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 6));
}

void BeginCard(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,  ImGui::ColorConvertU32ToFloat4(kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImGui::ColorConvertU32ToFloat4(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild(title, ImVec2(0, 0), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + 3, p.y + 16), kAccent, 2.0f);
    ImGui::Dummy(ImVec2(8, 0));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextHi));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, 4));
}

bool RailButton(const char* glyph, const char* label, bool active, int height = 52) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;

    ImVec2 pos  = ImGui::GetCursorScreenPos();
    float  w    = ImGui::GetContentRegionAvail().x;
    ImVec2 size(w, (float)height);
    ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGuiID id = win->GetID(label);
    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    ImGui::ItemSize(size);
    ImGui::ItemAdd(bb, id);

    ImU32 bg = active ? IM_COL32(26, 46, 50, 255)
              : hovered ? IM_COL32(26, 31, 39, 255)
              : IM_COL32(0, 0, 0, 0);
    dl->AddRectFilled(bb.Min, bb.Max, bg, 8.0f);
    if (active)
        dl->AddRectFilled(ImVec2(bb.Min.x + 4, bb.Min.y + 10),
                          ImVec2(bb.Min.x + 7, bb.Max.y - 10),
                          kAccent, 2.0f);

    ImU32 textCol = active ? kAccentHi : (hovered ? kTextHi : kTextLo);
    ImVec2 glyphSz = ImGui::CalcTextSize(glyph);
    ImVec2 labelSz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(bb.Min.x + 20, bb.Min.y + (size.y - glyphSz.y - labelSz.y - 2) * 0.5f),
                textCol, glyph);
    dl->AddText(ImVec2(bb.Min.x + 20 + glyphSz.x + 10,
                       bb.Min.y + (size.y - labelSz.y) * 0.5f),
                textCol, label);
    return pressed;
}

bool AccentButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::ColorConvertU32ToFloat4(IM_COL32(32, 72, 70, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(IM_COL32(50, 110, 105, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::ColorConvertU32ToFloat4(kAccentDim));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImGui::ColorConvertU32ToFloat4(kAccentHi));
    bool r = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return r;
}

void StatRow(const char* k, const char* v) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextLo));
    ImGui::Text("%s", k);
    ImGui::PopStyleColor();
    ImGui::SameLine(140);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextHi));
    ImGui::Text("%s", v);
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------
// Sections
// ------------------------------------------------------------------
void SecAim() {
    SectionTitle("Aim", "// legit / rage / target");
    auto& a = Aimbot::Cfg();
    BeginCard("MODE");
    const char* modes[] = { "Off", "Legit", "Rage" };
    int mi = (int)a.mode;
    if (ImGui::Combo("Mode", &mi, modes, 3)) a.mode = (Aimbot::Mode)mi;
    static bool bindPend = false;
    BindButton("Aim key", &a.keyVk, &bindPend);
    ImGui::Checkbox("Sticky target",      &a.sticky);
    ImGui::Checkbox("Ignore dead",        &a.ignoreDead);
    ImGui::Checkbox("Draw FOV",           &a.drawFov);
    ImGui::Checkbox("Draw aim line",      &a.drawAimLine);
    EndCard();

    BeginCard("TARGET");
    const char* bones[] = { "Head", "Torso", "Legs", "Nearest" };
    int bi = (int)a.bone;
    if (ImGui::Combo("Bone", &bi, bones, 4)) a.bone = (Aimbot::Bone)bi;
    const char* multis[] = { "Single", "Head + Torso", "Full rig" };
    int mu = (int)a.multi;
    if (ImGui::Combo("Multipoint", &mu, multis, 3)) a.multi = (Aimbot::Multi)mu;
    const char* prios[] = { "Closest to crosshair", "Closest in world" };
    int pi = (int)a.priority;
    if (ImGui::Combo("Priority", &pi, prios, 2)) a.priority = (Aimbot::Priority)pi;
    EndCard();

    BeginCard("RESPONSE");
    ImGui::SliderFloat("FOV (px)",        &a.fov,          20.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("Smoothing",       &a.smooth,       1.0f,  20.0f);
    ImGui::SliderFloat("Max distance",    &a.maxDistance,  10.0f, 500.0f);
    ImGui::SliderFloat("Safe-point edge", &a.safePointEdge,0.0f,  120.0f);
    EndCard();

    BeginCard("PREDICTION");
    ImGui::Checkbox("Lead prediction",    &a.predict);
    ImGui::SliderFloat("Bullet speed",    &a.bulletSpeed,  50.0f, 1500.0f);
    EndCard();

    BeginCard("RAGE");
    ImGui::Checkbox("Auto fire",          &a.autoFire);
    ImGui::SliderInt ("Lock dwell (frames)", &a.lockDwell, 1, 20);
    ImGui::SliderInt ("Baim HP above",       &a.baimHpAbove, 0, 100);
    ImGui::Checkbox("Memory aim (writes camera basis)", &a.memoryAim);
    EndCard();

    BeginCard("RCS");
    ImGui::Checkbox("Recoil control",     &a.rcs);
    ImGui::SliderFloat("Strength",        &a.rcsStrength, 0.0f, 1.5f);
    EndCard();
}

void SecTrigger() {
    SectionTitle("Trigger", "// autoshoot on crosshair target");
    auto& t = Aimbot::TrigCfg();
    BeginCard("TRIGGER");
    ImGui::Checkbox("Enable",             &t.enabled);
    static bool pend = false;
    BindButton("Trigger key", &t.keyVk, &pend);
    ImGui::Checkbox("Only when aim-key held", &t.onlyWhenAiming);
    ImGui::SliderInt ("Delay (ms)",       &t.delayMs, 0, 500);
    ImGui::SliderInt ("Burst limit (0 = inf)", &t.burstMax, 0, 20);
    ImGui::SliderInt ("FOV (px)",         &t.fovPx, 1, 200);
    ImGui::Checkbox("Head only",          &t.headOnly);
    ImGui::Checkbox("Body only",          &t.bodyOnly);
    EndCard();
}

void SecVisuals() {
    SectionTitle("Visuals", "// box / name / radar");
    auto& c = ESP::Cfg();
    BeginCard("MASTER");
    ImGui::Checkbox("ESP master",         &c.enabled);
    ImGui::Checkbox("Show dead",          &c.showDead);
    ImGui::Checkbox("Show offscreen",     &c.showOffscreen);
    ImGui::SliderFloat("Max distance",    &c.maxDistance, 10.0f, 1000.0f);
    ImGui::SliderFloat("Box scale",       &c.boxScale, 0.3f, 2.0f);
    EndCard();

    BeginCard("BOX & LABELS");
    ImGui::Checkbox("Box",                &c.box);
    ImGui::Checkbox("Corner box",         &c.cornerBox);
    ImGui::Checkbox("Snapline",           &c.snapline);
    ImGui::Checkbox("Health bar",         &c.healthBar);
    ImGui::Checkbox("Name tag",           &c.nameTag);
    ImGui::Checkbox("Distance tag",       &c.distanceTag);
    ImGui::Checkbox("Role tag",           &c.roleTag);
    ImGui::Checkbox("Held item",          &c.heldItemTag);
    ImGui::Checkbox("Armor tag",          &c.armorTag);
    ImGui::Checkbox("Clan tag",           &c.clanTag);
    EndCard();

    BeginCard("BODY");
    ImGui::Checkbox("Portrait",           &c.portrait);
    ImGui::Checkbox("Head dot",           &c.headDot);
    ImGui::Checkbox("Skeleton",           &c.skeleton);
    ImGui::Checkbox("Chams wire",         &c.chamsWire);
    EndCard();

    BeginCard("CROSSHAIR");
    ImGui::Checkbox("Enabled",            &c.crosshair);
    const char* styles[] = { "Dot", "Cross", "T-Cross", "Circle", "Bracket", "Plus" };
    int cs = (int)c.crosshairStyle;
    if (ImGui::Combo("Style", &cs, styles, 6)) c.crosshairStyle = (ESP::CrosshairStyle)cs;
    ImGui::SliderFloat("Size",      &c.crosshairSize,  2.0f, 24.0f);
    ImGui::SliderFloat("Gap",       &c.crosshairGap,   0.0f,  8.0f);
    ImGui::SliderFloat("Thickness", &c.crosshairThick, 1.0f,  4.0f);
    EndCard();

    BeginCard("HUD");
    ImGui::Checkbox("Watermark",          &c.watermark);
    ImGui::Checkbox("Radar",              &c.radar);
    ImGui::Checkbox("Radar loot",         &c.radarLoot);
    ImGui::SliderFloat("Radar size",      &c.radarSize, 100.0f, 400.0f);
    ImGui::SliderFloat("Radar range",     &c.radarRange, 50.0f, 800.0f);
    EndCard();
}

void SecPlayers() {
    SectionTitle("Players", "// live roster");
    BeginCard("ROSTER");
    std::vector<Player> players;
    Game::SnapshotPlayers(players);
    if (players.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextLo));
        ImGui::TextUnformatted("no entities yet — waiting on root discovery...");
        ImGui::PopStyleColor();
    }
    for (auto& p : players) {
        ImGui::PushID((int)p.entityId);
        ImU32 pill = p.isLocalPlayer ? kAccent : (p.alive ? IM_COL32(230, 90, 100, 255)
                                                           : IM_COL32(110, 120, 135, 255));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(cp.x, cp.y + 4), ImVec2(cp.x + 4, cp.y + 20), pill, 2.0f);
        ImGui::Dummy(ImVec2(10, 0));
        ImGui::SameLine();
        ImGui::Text("%-18s", p.name.empty() ? "unknown" : p.name.c_str());
        ImGui::SameLine(200);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextLo));
        ImGui::Text("hp %-3d  %4dm  %s", p.health, (int)p.distance,
                    p.role.empty() ? "" : p.role.c_str());
        ImGui::PopStyleColor();
        if (!p.isLocalPlayer) {
            ImGui::SameLine(410);
            if (ImGui::SmallButton("TP"))   Game::BeginTeleport(Game::TpKind::ToEntity,   p.entityId, p.root);
            ImGui::SameLine();
            if (ImGui::SmallButton("Pull")) Game::BeginTeleport(Game::TpKind::PullEntity, p.entityId, p.root);
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (AccentButton("Cancel teleport")) Game::CancelTeleport();
    EndCard();
}

void SecWorld() {
    SectionTitle("World", "// loot / vehicles / doors");
    auto& c = ESP::Cfg();
    BeginCard("LOOT ESP");
    ImGui::Checkbox("Enabled",            &c.lootEsp);
    ImGui::SliderInt("Min rarity",        &c.lootMinRarity, 0, 6);
    ImGui::SliderFloat("Max distance",    &c.lootMaxDistance, 10.0f, 500.0f);
    EndCard();

    BeginCard("WORLD MARKS");
    ImGui::Checkbox("Vehicle ESP",        &c.vehicleEsp);
    ImGui::Checkbox("Door ESP",           &c.doorEsp);
    EndCard();
}

void SecMisc() {
    SectionTitle("Misc", "// movement / view / probe");
    auto& m = Misc::Cfg();
    BeginCard("MOVEMENT");
    static bool fly = false;
    if (ImGui::Checkbox("Fly mode", &fly)) {
        if (fly) {
            Cam cam = Game::Camera();
            Game::BeginTeleport(Game::TpKind::ToPoint, 0, cam.pos, true);
        } else {
            Game::CancelTeleport();
        }
    }
    static bool bhBind = false;
    ImGui::Checkbox("Bunny hop",          &m.bhop);
    BindButton("Jump key", &m.bhopKey, &bhBind);
    ImGui::Separator();
    ImGui::Checkbox("Fast duck",          &m.fastDuck);
    ImGui::SliderInt("Duck rate (Hz)",    &m.fastDuckHz, 2, 30);
    ImGui::Checkbox("Fake duck (write)",  &m.fakeDuck);
    ImGui::Separator();
    ImGui::Checkbox("Airstuck",           &m.airstuck);
    static bool asBind = false;
    BindButton("Airstuck key", &m.airstuckKey, &asBind);
    EndCard();

    BeginCard("VIEW");
    ImGui::Checkbox("Third person",       &m.thirdPerson);
    ImGui::SliderFloat("Distance",        &m.thirdDistance, 0.5f, 10.0f);
    ImGui::SliderFloat("Height",          &m.thirdHeight,  -2.0f,  5.0f);
    ImGui::Separator();
    ImGui::Checkbox("FOV override",       &m.fovOverride);
    ImGui::SliderFloat("FOV",             &m.fovDeg, 40.0f, 140.0f);
    EndCard();

    BeginCard("GUN MODS");
    static Game::GunToggles gun;
    bool ch = false;
    ch |= ImGui::Checkbox("No recoil",    &gun.noRecoil);
    ch |= ImGui::Checkbox("No spread",    &gun.noSpread);
    ch |= ImGui::Checkbox("No sway",      &gun.noSway);
    if (ch) Game::ApplyGun(gun);
    EndCard();

    BeginCard("PROBE");
    char buf[48];
    std::snprintf(buf, 48, "%.1f deg", Game::Camera().fov);
    StatRow("Camera FOV",   buf);
    std::snprintf(buf, 48, "%dx%d", Overlay::Width(), Overlay::Height());
    StatRow("Screen",       buf);
    Cam c = Game::Camera();
    StatRow("Status",       c.valid ? "ok" : "no camera");
    EndCard();
}

void SecSettings() {
    SectionTitle("Config", "// profiles / palette / binds");
    BeginCard("PROFILES");
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        bool active = (g_activeProfile == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                        ImGui::ColorConvertU32ToFloat4(IM_COL32(32, 72, 70, 255)));
        char tag[24];
        std::snprintf(tag, 24, "slot %s", g_profileNames[i]);
        if (ImGui::Button(tag, ImVec2(70, 28))) g_activeProfile = i;
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##name", g_profileNames[i], 16);
        ImGui::SameLine();
        if (AccentButton("Save")) { Menu::SaveConfig(); }
        ImGui::SameLine();
        if (AccentButton("Load")) { g_activeProfile = i; Menu::LoadConfig(); }
        ImGui::PopID();
    }
    EndCard();

    auto& c = ESP::Cfg();
    BeginCard("PALETTE");
    auto colorPick = [](const char* label, uint32_t& v) {
        ImVec4 iv = ImVecFromColor(v);
        if (ImGui::ColorEdit4(label, (float*)&iv, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
            v = ColorFromImVec(iv);
    };
    colorPick("Enemy",   c.colorEnemy);
    colorPick("Ally",    c.colorAlly);
    colorPick("Dead",    c.colorDead);
    colorPick("OwnTeam", c.colorOwnTeam);
    colorPick("Text",    c.colorText);
    EndCard();
}

// ------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------
struct Section { const char* glyph; const char* label; void (*draw)(); };
static const Section kSections[] = {
    { "[a]", "Aim",      SecAim      },
    { "[t]", "Trigger",  SecTrigger  },
    { "[v]", "Visuals",  SecVisuals  },
    { "[p]", "Players",  SecPlayers  },
    { "[w]", "World",    SecWorld    },
    { "[m]", "Misc",     SecMisc     },
    { "[c]", "Config",   SecSettings },
};

void DrawTitleBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + 46), kPanelHi, 10.0f);
    dl->AddRectFilled(ImVec2(p.x, p.y + 44), ImVec2(p.x + w, p.y + 46),
                      kAccent, 0.0f);
    // brand dot
    dl->AddCircleFilled(ImVec2(p.x + 22, p.y + 23), 5.0f, kAccent, 12);
    dl->AddCircle      (ImVec2(p.x + 22, p.y + 23), 8.0f, kAccentDim, 24, 1.5f);
    dl->AddText(ImVec2(p.x + 42, p.y + 10), kTextHi, "loko");
    dl->AddText(ImVec2(p.x + 42 + 36, p.y + 10), kAccent, ".gg");
    dl->AddText(ImVec2(p.x + 42, p.y + 27), kTextLo, "external // rob run");
    char buf[48];
    Cam c = Game::Camera();
    std::snprintf(buf, 48, c.valid ? "connected" : "linking...");
    ImVec2 sz = ImGui::CalcTextSize(buf);
    dl->AddRectFilled(ImVec2(p.x + w - sz.x - 24, p.y + 14),
                      ImVec2(p.x + w - 12,         p.y + 32),
                      c.valid ? IM_COL32(32, 72, 70, 255) : IM_COL32(60, 44, 50, 255),
                      5.0f);
    dl->AddCircleFilled(ImVec2(p.x + w - sz.x - 18, p.y + 23), 3.0f,
                        c.valid ? kAccent : IM_COL32(230, 90, 100, 255), 12);
    dl->AddText(ImVec2(p.x + w - sz.x - 10, p.y + 16),
                c.valid ? kAccentHi : IM_COL32(230, 90, 100, 255), buf);
    ImGui::Dummy(ImVec2(w, 46));
    ImGui::Dummy(ImVec2(0, 6));
}

} // namespace

void Menu::Init() {}

void Menu::Toggle() {
    g_open = !g_open;
    Overlay::SetClickThrough(!g_open);
}

bool Menu::IsOpen() { return g_open; }

void Menu::Render() {
    if (!g_open) return;
    ImGui::SetNextWindowSize(ImVec2(760, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(140, 110), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
    if (ImGui::Begin("loko.gg", &g_open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar))
    {
        DrawTitleBar();

        // Left rail
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kPanel));
        ImGui::PushStyleColor(ImGuiCol_Border,  ImGui::ColorConvertU32ToFloat4(kBorder));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::BeginChild("rail", ImVec2(170, 0), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        for (int i = 0; i < (int)(sizeof(kSections)/sizeof(kSections[0])); ++i) {
            if (RailButton(kSections[i].glyph, kSections[i].label, g_section == i))
                g_section = i;
        }
        // footer: hotkey hint
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextLo));
        ImGui::TextWrapped("INS   toggle\nEND   quit");
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        // Right content
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(IM_COL32(15, 17, 21, 255)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::BeginChild("content", ImVec2(0, 0), false);
        if (g_section >= 0 && g_section < (int)(sizeof(kSections)/sizeof(kSections[0])))
            kSections[g_section].draw();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar();

    Overlay::SetClickThrough(!ImGui::GetIO().WantCaptureMouse);
}

static void ProfilePath(char* out, size_t n) {
    char slot = 'A' + (char)(g_activeProfile % 3);
    std::snprintf(out, n, "rob-run.%c.cfg", slot);
}

void Menu::SaveConfig() {
    SavedBlob b;
    b.esp  = ESP::Cfg();
    b.aim  = Aimbot::Cfg();
    b.trig = Aimbot::TrigCfg();
    b.misc = Misc::Cfg();
    char path[64]; ProfilePath(path, sizeof(path));
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return;
    std::fwrite(&b, sizeof(b), 1, f);
    std::fclose(f);
    Console::Success("config saved -> %s", path);
}

void Menu::LoadConfig() {
    char path[64]; ProfilePath(path, sizeof(path));
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f) return;
    SavedBlob b;
    if (std::fread(&b, sizeof(b), 1, f) == 1 && b.magic == 0x4B4F4C52 && b.version == 3) {
        ESP::Cfg()        = b.esp;
        Aimbot::Cfg()     = b.aim;
        Aimbot::TrigCfg() = b.trig;
        Misc::Cfg()       = b.misc;
        Console::Success("config loaded <- %s", path);
    }
    std::fclose(f);
}
