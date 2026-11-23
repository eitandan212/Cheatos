#include "menu.h"
#include "esp.h"
#include "aimbot.h"
#include "game.h"
#include "overlay.h"
#include "console.h"

#include "../imgui/imgui.h"
#include <cstdio>
#include <cstdint>

namespace {

bool g_open = false;

struct SavedBlob {
    uint32_t magic = 0x4B4F4C52; // 'RLOK'
    uint32_t version = 1;
    ESP::Settings    esp;
    Aimbot::Settings aim;
};

void BeginCard(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.03f, 0.03f, 0.95f));
    ImGui::BeginChild(title, ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.00f), "%s", title);
    ImGui::Separator();
}
void EndCard() { ImGui::EndChild(); ImGui::PopStyleColor(); }

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

void TabAim() {
    auto& a = Aimbot::Cfg();
    BeginCard("Aimbot");
    ImGui::Checkbox("Enable",        &a.enabled);
    ImGui::Checkbox("Sticky target", &a.sticky);
    ImGui::Checkbox("Ignore dead",   &a.ignoreDead);
    ImGui::Checkbox("Draw FOV",      &a.drawFov);
    ImGui::Checkbox("Lead prediction",&a.predict);
    ImGui::Checkbox("Memory aim",    &a.memoryAim);
    ImGui::Checkbox("Recoil compensation", &a.recoilComp);
    ImGui::SliderFloat("FOV (px)",   &a.fov,         20.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("Smoothing",  &a.smooth,      1.0f,  20.0f);
    ImGui::SliderFloat("Max dist",   &a.maxDistance, 10.0f, 500.0f);
    ImGui::SliderFloat("Bullet speed",&a.bulletSpeed,50.0f,1500.0f);
    const char* bones[] = { "Head", "Torso", "Legs", "Nearest" };
    int bi = (int)a.bone;
    if (ImGui::Combo("Bone", &bi, bones, 4)) a.bone = (Aimbot::Bone)bi;
    const char* prios[] = { "Closest to crosshair", "Closest in world" };
    int pi = (int)a.priority;
    if (ImGui::Combo("Priority", &pi, prios, 2)) a.priority = (Aimbot::Priority)pi;
    EndCard();
}

void TabVisuals() {
    auto& c = ESP::Cfg();
    BeginCard("Visuals");
    ImGui::Checkbox("ESP master",    &c.enabled);
    ImGui::Checkbox("Show dead",     &c.showDead);
    ImGui::Checkbox("Show offscreen",&c.showOffscreen);
    ImGui::SliderFloat("Max dist",   &c.maxDistance, 10.0f, 1000.0f);
    ImGui::SliderFloat("Box scale",  &c.boxScale, 0.3f, 2.0f);
    ImGui::Separator();
    ImGui::Checkbox("Box",           &c.box);
    ImGui::Checkbox("Corner box",    &c.cornerBox);
    ImGui::Checkbox("Snapline",      &c.snapline);
    ImGui::Checkbox("Health bar",    &c.healthBar);
    ImGui::Checkbox("Name",          &c.nameTag);
    ImGui::Checkbox("Distance",      &c.distanceTag);
    ImGui::Checkbox("Role",          &c.roleTag);
    ImGui::Checkbox("Held item",     &c.heldItemTag);
    ImGui::Checkbox("Armor",         &c.armorTag);
    ImGui::Checkbox("Clan",          &c.clanTag);
    ImGui::Checkbox("Portrait",      &c.portrait);
    ImGui::Checkbox("Head dot",      &c.headDot);
    ImGui::Checkbox("Skeleton",      &c.skeleton);
    ImGui::Checkbox("Chams wire",    &c.chamsWire);
    ImGui::Checkbox("Crosshair",     &c.crosshair);
    ImGui::Checkbox("Watermark",     &c.watermark);
    EndCard();
}

void TabPlayers() {
    BeginCard("Players");
    std::vector<Player> players;
    Game::SnapshotPlayers(players);
    for (auto& p : players) {
        if (p.isLocalPlayer) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "YOU  %s  hp=%d",
                               p.name.c_str(), p.health);
            continue;
        }
        ImGui::PushID((int)p.entityId);
        ImGui::Text("%-18s  hp=%-3d  d=%4dm  %s",
                    p.name.c_str(), p.health, (int)p.distance,
                    p.role.empty() ? "" : p.role.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("TP to"))  Game::BeginTeleport(Game::TpKind::ToEntity, p.entityId, p.root);
        ImGui::SameLine();
        if (ImGui::SmallButton("Pull"))   Game::BeginTeleport(Game::TpKind::PullEntity, p.entityId, p.root);
        ImGui::PopID();
    }
    if (ImGui::Button("Cancel teleport")) Game::CancelTeleport();
    EndCard();
}

void TabMisc() {
    BeginCard("Misc");
    static bool fly = false;
    if (ImGui::Checkbox("Fly mode", &fly)) {
        if (fly) {
            Cam c = Game::Camera();
            Game::BeginTeleport(Game::TpKind::ToPoint, 0, c.pos, true);
        } else {
            Game::CancelTeleport();
        }
    }
    static Game::GunToggles gun;
    bool changed = false;
    changed |= ImGui::Checkbox("No recoil", &gun.noRecoil);
    changed |= ImGui::Checkbox("No spread", &gun.noSpread);
    changed |= ImGui::Checkbox("No sway",   &gun.noSway);
    if (changed) Game::ApplyGun(gun);
    ImGui::Text("Camera FOV: %.1f", Game::Camera().fov);
    ImGui::Text("Screen: %dx%d", Overlay::Width(), Overlay::Height());
    EndCard();
}

void TabSettings() {
    auto& c = ESP::Cfg();
    BeginCard("Settings");
    if (ImGui::Button("Save config")) Menu::SaveConfig();
    ImGui::SameLine();
    if (ImGui::Button("Load config")) Menu::LoadConfig();
    ImGui::Separator();
    auto colorPick = [](const char* label, uint32_t& c) {
        ImVec4 v = ImVecFromColor(c);
        if (ImGui::ColorEdit4(label, (float*)&v, ImGuiColorEditFlags_NoInputs))
            c = ColorFromImVec(v);
    };
    colorPick("Enemy",   c.colorEnemy);
    colorPick("Ally",    c.colorAlly);
    colorPick("Dead",    c.colorDead);
    colorPick("OwnTeam", c.colorOwnTeam);
    colorPick("Text",    c.colorText);
    ImGui::Separator();
    ImGui::Checkbox("Radar",           &c.radar);
    ImGui::Checkbox("Radar loot",      &c.radarLoot);
    ImGui::SliderFloat("Radar size",   &c.radarSize, 100.0f, 400.0f);
    ImGui::SliderFloat("Radar range",  &c.radarRange,50.0f,  800.0f);
    ImGui::Separator();
    ImGui::Checkbox("Loot ESP",        &c.lootEsp);
    ImGui::SliderInt("Loot min rarity",&c.lootMinRarity, 0, 6);
    ImGui::SliderFloat("Loot max dist",&c.lootMaxDistance, 10.0f, 500.0f);
    ImGui::Checkbox("Vehicle ESP",     &c.vehicleEsp);
    ImGui::Checkbox("Door ESP",        &c.doorEsp);
    EndCard();
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
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(120, 120), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("loko loko", &g_open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Aimbot"))   { TabAim();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Visuals"))  { TabVisuals(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Players"))  { TabPlayers(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Misc"))     { TabMisc();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings")) { TabSettings();ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    // click-through follows what ImGui is actually under
    Overlay::SetClickThrough(!ImGui::GetIO().WantCaptureMouse);
}

void Menu::SaveConfig() {
    SavedBlob b;
    b.esp = ESP::Cfg();
    b.aim = Aimbot::Cfg();
    FILE* f = nullptr;
    fopen_s(&f, "rob-run.cfg", "wb");
    if (!f) return;
    std::fwrite(&b, sizeof(b), 1, f);
    std::fclose(f);
    Console::Success("config saved");
}

void Menu::LoadConfig() {
    FILE* f = nullptr;
    fopen_s(&f, "rob-run.cfg", "rb");
    if (!f) return;
    SavedBlob b;
    if (std::fread(&b, sizeof(b), 1, f) == 1 && b.magic == 0x4B4F4C52 && b.version == 1) {
        ESP::Cfg()    = b.esp;
        Aimbot::Cfg() = b.aim;
        Console::Success("config loaded");
    }
    std::fclose(f);
}
