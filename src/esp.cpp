#include "esp.h"
#include "aimbot.h"
#include "game.h"
#include "overlay.h"
#include "portrait.h"
#include "console.h"
#include "state.h"

#include "../imgui/imgui.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace {

ESP::Settings g_cfg;

uint32_t HealthColor(int hp, int maxHp) {
    // klar-brand ramp: coral (low) -> amber (mid) -> mint (high)
    if (maxHp <= 0) maxHp = 100;
    float f = (float)std::clamp(hp, 0, maxHp) / (float)maxHp;
    auto lerp = [](int a, int b, float t) { return (int)(a + (b - a) * t); };
    int r, g, b;
    if (f >= 0.5f) {
        float t = (f - 0.5f) * 2.0f;
        r = lerp(0xE6, 0x55, t);
        g = lerp(0xB0, 0xE3, t);
        b = lerp(0x4A, 0x9A, t);
    } else {
        float t = f * 2.0f;
        r = lerp(0xE3, 0xE6, t);
        g = lerp(0x4D, 0xB0, t);
        b = lerp(0x5C, 0x4A, t);
    }
    return IM_COL32(r, g, b, 255);
}

uint32_t RarityColor(int r) {
    switch (r) {
        case 1: return IM_COL32(180, 180, 180, 255);
        case 2: return IM_COL32(60,  220, 90,  255);
        case 3: return IM_COL32(70,  160, 255, 255);
        case 4: return IM_COL32(180, 90,  255, 255);
        case 5: return IM_COL32(255, 160, 60,  255);
        case 6: return IM_COL32(255, 50,  50,  255);
        default:return IM_COL32(220, 220, 220, 255);
    }
}

void DrawShadowText(ImDrawList* dl, ImVec2 p, uint32_t col, const char* txt) {
    dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 220), txt);
    dl->AddText(p, col, txt);
}

void DrawCorners(ImDrawList* dl, float x, float y, float w, float h, uint32_t col) {
    float cx = w * 0.25f, cy = h * 0.25f;
    uint32_t sh = IM_COL32(0, 0, 0, 220);
    // top-left
    dl->AddLine(ImVec2(x + 1,      y + 1),      ImVec2(x + 1 + cx, y + 1),      sh, 1.5f);
    dl->AddLine(ImVec2(x + 1,      y + 1),      ImVec2(x + 1,      y + 1 + cy), sh, 1.5f);
    dl->AddLine(ImVec2(x,          y),          ImVec2(x + cx,     y),          col, 1.5f);
    dl->AddLine(ImVec2(x,          y),          ImVec2(x,          y + cy),     col, 1.5f);
    // top-right
    dl->AddLine(ImVec2(x + w,      y),          ImVec2(x + w - cx, y),          col, 1.5f);
    dl->AddLine(ImVec2(x + w,      y),          ImVec2(x + w,      y + cy),     col, 1.5f);
    // bottom-left
    dl->AddLine(ImVec2(x,          y + h),      ImVec2(x + cx,     y + h),      col, 1.5f);
    dl->AddLine(ImVec2(x,          y + h),      ImVec2(x,          y + h - cy), col, 1.5f);
    // bottom-right
    dl->AddLine(ImVec2(x + w,      y + h),      ImVec2(x + w - cx, y + h),      col, 1.5f);
    dl->AddLine(ImVec2(x + w,      y + h),      ImVec2(x + w,      y + h - cy), col, 1.5f);
}

void DrawHealthBar(ImDrawList* dl, float x, float y, float h, int hp, int maxHp) {
    float f = (float)std::clamp(hp, 0, maxHp) / (float)std::max(1, maxHp);
    float bw = 3.0f;
    dl->AddRectFilled(ImVec2(x - bw - 2, y), ImVec2(x - 2, y + h), IM_COL32(0, 0, 0, 180));
    float fill = h * f;
    dl->AddRectFilled(ImVec2(x - bw - 2, y + (h - fill)), ImVec2(x - 2, y + h),
                      HealthColor(hp, maxHp));
}

void DrawSkeleton(ImDrawList* dl, const Player& p, uint32_t col) {
    if (!p.hasRig) return;
    auto proj = [&](const Vec3& v, ImVec2& out) -> bool {
        float s[2];
        if (!Game::WorldToScreen(v, s)) return false;
        out = ImVec2(s[0], s[1]);
        return true;
    };
    ImVec2 sp[16];
    bool ok[16] = {};
    for (int i = 0; i < 16; ++i) ok[i] = proj(p.rig.joints[i], sp[i]);
    static const int edges[][2] = {
        {0,1},{1,2},{2,3},
        {1,4},{4,6},{6,8},
        {1,5},{5,7},{7,9},
        {0,10},{10,12},{12,14},
        {0,11},{11,13},{13,15},
    };
    for (auto& e : edges) {
        if (ok[e[0]] && ok[e[1]]) dl->AddLine(sp[e[0]], sp[e[1]], col, 1.2f);
    }
}

void DrawChamsWire(ImDrawList* dl, const Player& p, uint32_t col) {
    if (!p.hasRig) return;
    auto proj = [&](const Vec3& v, ImVec2& out) -> bool {
        float s[2];
        if (!Game::WorldToScreen(v, s)) return false;
        out = ImVec2(s[0], s[1]); return true;
    };
    // three rings: foot / pelvis / chest / head approximated from joints
    Vec3 foot = Vec3((p.rig.joints[14].x + p.rig.joints[15].x) * 0.5f,
                     (p.rig.joints[14].y + p.rig.joints[15].y) * 0.5f,
                     (p.rig.joints[14].z + p.rig.joints[15].z) * 0.5f);
    Vec3 pelvis = p.rig.joints[0];
    Vec3 chest  = p.rig.joints[1];
    Vec3 head   = p.rig.joints[3];
    Vec3 rings[4] = { foot, pelvis, chest, head };
    const int sides = 6;
    for (int r = 0; r < 4; ++r) {
        ImVec2 ring[sides];
        bool ringOk[sides] = {};
        for (int i = 0; i < sides; ++i) {
            float ang = (float)i / sides * 6.2831853f;
            Vec3 pt(rings[r].x + 0.25f * std::cos(ang), rings[r].y, rings[r].z + 0.25f * std::sin(ang));
            ringOk[i] = proj(pt, ring[i]);
        }
        for (int i = 0; i < sides; ++i) {
            int j = (i + 1) % sides;
            if (ringOk[i] && ringOk[j]) dl->AddLine(ring[i], ring[j], col, 1.0f);
        }
    }
}

void DrawRadar(ImDrawList* dl, const std::vector<Player>& players, const std::vector<LootItem>& loot) {
    Cam c = Game::Camera();
    if (!c.valid) return;
    float size = g_cfg.radarSize;
    float cx = (float)Overlay::Width() - size * 0.5f - 24.0f;
    float cy = size * 0.5f + 24.0f;
    dl->AddCircleFilled(ImVec2(cx, cy), size * 0.5f, IM_COL32(0, 0, 0, 140), 48);
    dl->AddCircle      (ImVec2(cx, cy), size * 0.5f, IM_COL32(200, 40, 40, 220), 48, 1.5f);
    float yaw = std::atan2(c.fwd.x, c.fwd.z);
    float ca = std::cos(-yaw), sa = std::sin(-yaw);
    float sc = size / (g_cfg.radarRange * 2.0f);
    auto plot = [&](const Vec3& pos, uint32_t col, float rad) {
        float dx = pos.x - c.pos.x;
        float dz = pos.z - c.pos.z;
        float rx = dx * ca - dz * sa;
        float ry = dx * sa + dz * ca;
        float px = cx + rx * sc;
        float py = cy - ry * sc;
        if (std::hypot(px - cx, py - cy) < size * 0.5f)
            dl->AddCircleFilled(ImVec2(px, py), rad, col, 12);
    };
    // camera indicator
    dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy - 10), IM_COL32(255,255,255,200), 1.5f);
    // players
    for (auto& p : players) {
        if (p.isLocalPlayer) continue;
        uint32_t col = p.alive ? g_cfg.colorEnemy : g_cfg.colorDead;
        plot(p.root, col, 3.5f);
    }
    if (g_cfg.radarLoot) {
        for (auto& li : loot) plot(li.pos, RarityColor(li.rarity), 1.5f);
    }
}

void DrawFar(ImDrawList* dl, const Vec3& w, uint32_t col) {
    float s[2]; float depth = 0;
    if (!Game::Project(w, s, &depth)) return;
    float W = (float)Overlay::Width();
    float H = (float)Overlay::Height();
    float cx = W * 0.5f;
    float cy = H * 0.5f;
    if (g_cfg.offscreenChevron) {
        float dx = s[0] - cx;
        float dy = s[1] - cy;
        float len = std::hypot(dx, dy);
        if (len < 1.0f) return;
        dx /= len; dy /= len;
        float r = std::min(W, H) * 0.42f;
        float tx = cx + dx * r;
        float ty = cy + dy * r;
        float nx = -dy, ny = dx;
        ImVec2 p0(tx + dx * 14, ty + dy * 14);
        ImVec2 p1(tx - dx *  4 + nx * 10, ty - dy *  4 + ny * 10);
        ImVec2 p2(tx - dx *  4 - nx * 10, ty - dy *  4 - ny * 10);
        dl->AddTriangleFilled(p0, p1, p2, col);
        dl->AddTriangle      (p0, p1, p2, IM_COL32(0, 0, 0, 200), 1.0f);
    } else {
        float x = std::clamp(s[0], 20.0f, W - 20.0f);
        float y = std::clamp(s[1], 20.0f, H - 20.0f);
        dl->AddCircleFilled(ImVec2(x, y), 4.0f, col, 12);
        dl->AddCircle      (ImVec2(x, y), 6.0f, IM_COL32(0, 0, 0, 220), 12, 1.5f);
    }
}

void DrawLootItem(ImDrawList* dl, const LootItem& li) {
    float s[2];
    if (!Game::WorldToScreen(li.pos, s)) return;
    uint32_t col = RarityColor(li.rarity);
    ImVec2 c(s[0], s[1]);
    ImVec2 pts[4] = {
        ImVec2(c.x,     c.y - 5),
        ImVec2(c.x + 5, c.y),
        ImVec2(c.x,     c.y + 5),
        ImVec2(c.x - 5, c.y),
    };
    dl->AddQuadFilled(pts[0], pts[1], pts[2], pts[3], col);
    dl->AddQuad      (pts[0], pts[1], pts[2], pts[3], IM_COL32(0,0,0,220), 1.0f);
    char buf[128];
    std::snprintf(buf, 128, "%s %dm", li.name.empty() ? "loot" : li.name.c_str(), (int)li.distance);
    DrawShadowText(dl, ImVec2(c.x + 8, c.y - 6), col, buf);
}

void DrawMark(ImDrawList* dl, const WorldMark& m) {
    float s[2];
    if (!Game::WorldToScreen(m.pos, s)) return;
    if (m.kind == WorldMark::Door) {
        dl->AddCircle(ImVec2(s[0], s[1]), 5.0f, IM_COL32(60, 220, 240, 255), 12, 1.5f);
    } else {
        ImVec2 pts[3] = {
            ImVec2(s[0],     s[1] - 7),
            ImVec2(s[0] + 7, s[1] + 5),
            ImVec2(s[0] - 7, s[1] + 5),
        };
        dl->AddTriangleFilled(pts[0], pts[1], pts[2], IM_COL32(240, 220, 60, 220));
    }
    char buf[64];
    std::snprintf(buf, 64, "%s %dm",
                  m.kind == WorldMark::Door ? "door" :
                  m.kind == WorldMark::Helicopter ? "heli" : "car",
                  (int)m.distance);
    DrawShadowText(dl, ImVec2(s[0] + 10, s[1] - 6), IM_COL32(230, 230, 230, 255), buf);
}

void DrawCrosshair(ImDrawList* dl) {
    float cx = (float)Overlay::Width() * 0.5f;
    float cy = (float)Overlay::Height() * 0.5f;
    const ImU32 col = IM_COL32(0x4C, 0xE0, 0xD9, 230);
    float sz = g_cfg.crosshairSize;
    float gap = g_cfg.crosshairGap;
    float t = g_cfg.crosshairThick;
    switch (g_cfg.crosshairStyle) {
        case ESP::CrosshairStyle::Dot:
            dl->AddCircleFilled(ImVec2(cx, cy), std::max(1.5f, sz * 0.4f), col, 12);
            break;
        case ESP::CrosshairStyle::Cross:
            dl->AddLine(ImVec2(cx - sz, cy), ImVec2(cx - gap, cy), col, t);
            dl->AddLine(ImVec2(cx + gap, cy), ImVec2(cx + sz, cy), col, t);
            dl->AddLine(ImVec2(cx, cy - sz), ImVec2(cx, cy - gap), col, t);
            dl->AddLine(ImVec2(cx, cy + gap), ImVec2(cx, cy + sz), col, t);
            break;
        case ESP::CrosshairStyle::TCross:
            dl->AddLine(ImVec2(cx - sz, cy), ImVec2(cx - gap, cy), col, t);
            dl->AddLine(ImVec2(cx + gap, cy), ImVec2(cx + sz, cy), col, t);
            dl->AddLine(ImVec2(cx, cy + gap), ImVec2(cx, cy + sz), col, t);
            break;
        case ESP::CrosshairStyle::Circle:
            dl->AddCircle(ImVec2(cx, cy), sz, col, 24, t);
            dl->AddCircleFilled(ImVec2(cx, cy), 1.0f, col, 8);
            break;
        case ESP::CrosshairStyle::Bracket:
            dl->AddLine(ImVec2(cx - sz, cy - sz*0.3f), ImVec2(cx - sz, cy + sz*0.3f), col, t);
            dl->AddLine(ImVec2(cx - sz, cy - sz*0.3f), ImVec2(cx - sz*0.7f, cy - sz*0.3f), col, t);
            dl->AddLine(ImVec2(cx - sz, cy + sz*0.3f), ImVec2(cx - sz*0.7f, cy + sz*0.3f), col, t);
            dl->AddLine(ImVec2(cx + sz, cy - sz*0.3f), ImVec2(cx + sz, cy + sz*0.3f), col, t);
            dl->AddLine(ImVec2(cx + sz, cy - sz*0.3f), ImVec2(cx + sz*0.7f, cy - sz*0.3f), col, t);
            dl->AddLine(ImVec2(cx + sz, cy + sz*0.3f), ImVec2(cx + sz*0.7f, cy + sz*0.3f), col, t);
            break;
        case ESP::CrosshairStyle::Plus:
            dl->AddLine(ImVec2(cx - sz, cy), ImVec2(cx + sz, cy), col, t);
            dl->AddLine(ImVec2(cx, cy - sz), ImVec2(cx, cy + sz), col, t);
            dl->AddCircleFilled(ImVec2(cx, cy), 1.2f, IM_COL32(0, 0, 0, 220), 10);
            break;
    }
}

void DrawWatermark(ImDrawList* dl) {
    const ImU32 accent = IM_COL32(0x4C, 0xE0, 0xD9, 255);
    const ImU32 accH   = IM_COL32(0x7C, 0xF7, 0xF0, 255);
    const ImU32 textLo = IM_COL32(0x82, 0x8C, 0x9B, 255);
    const ImU32 panel  = IM_COL32(0x14, 0x17, 0x1D, 210);
    const ImU32 border = IM_COL32(0x2A, 0x31, 0x3D, 255);

    float x = 14.0f, y = 14.0f, w = 190.0f, h = 42.0f;
    dl->AddRectFilled(ImVec2(x, y),         ImVec2(x + w, y + h), panel,  8.0f);
    dl->AddRect      (ImVec2(x, y),         ImVec2(x + w, y + h), border, 8.0f, 0, 1.0f);
    dl->AddRectFilled(ImVec2(x + 2, y + h - 2), ImVec2(x + w - 2, y + h), accent, 2.0f);
    dl->AddCircleFilled(ImVec2(x + 14, y + h * 0.5f), 4.0f, accent, 12);
    dl->AddCircle      (ImVec2(x + 14, y + h * 0.5f), 7.0f, IM_COL32(0x32, 0xAF, 0xAA, 255), 24, 1.5f);
    dl->AddText(ImVec2(x + 28, y + 8),  IM_COL32(0xE6, 0xEA, 0xF0, 255), "loko");
    dl->AddText(ImVec2(x + 28 + 36, y + 8),  accH,  ".gg");
    dl->AddText(ImVec2(x + 28, y + 24), textLo, "external // rob run");
}

} // namespace

ESP::Settings& ESP::Cfg() { return g_cfg; }

void ESP::Render() {
    if (!g_cfg.enabled) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    std::vector<Player>    players;
    std::vector<LootItem>  loot;
    std::vector<WorldMark> marks;
    Game::SnapshotPlayers(players);
    Game::SnapshotLoot(loot);
    Game::SnapshotMarks(marks);

    if (g_cfg.watermark && !AppState::Cfg().streamerMode)  DrawWatermark(dl);
    if (g_cfg.crosshair)  DrawCrosshair(dl);

    auto& aim = Aimbot::Cfg();
    if (aim.mode != Aimbot::Mode::Off && aim.drawFov) {
        float cx = (float)Overlay::Width() * 0.5f;
        float cy = (float)Overlay::Height() * 0.5f;
        dl->AddCircle(ImVec2(cx, cy), aim.fov, IM_COL32(0x4C, 0xE0, 0xD9, 160), 64, 1.0f);
    }

    if (AppState::Cfg().panicActive) return;

    int vis = 0, far_ = 0;
    for (auto& p : players) {
        if (p.isLocalPlayer) continue;
        if (!p.alive && !g_cfg.showDead) continue;
        if (p.distance > g_cfg.maxDistance) continue;

        bool isFriend = AppState::IsFriend(p.name, p.clan);
        bool isWatch  = AppState::IsWatched(p.name, p.clan);

        float rootS[2], headS[2];
        bool rok = Game::WorldToScreen(p.root, rootS);
        bool hok = Game::WorldToScreen(p.head, headS);
        if (rok && hok) {
            vis++;
            float topY  = headS[1] - 6.0f;
            float botY  = rootS[1];
            float h = botY - topY;
            if (p.crouch)    h *= 0.7f;
            if (p.inVehicle) h *= 0.5f;
            float w = h * 0.55f * g_cfg.boxScale;
            float cx = (headS[0] + rootS[0]) * 0.5f;
            float x = cx - w * 0.5f;

            uint32_t col =
                !p.alive ? g_cfg.colorDead
                : isWatch  ? g_cfg.colorWatch
                : isFriend ? g_cfg.colorFriend
                : g_cfg.colorEnemy;

            switch (g_cfg.boxStyle) {
                case BoxStyle::Corner:   DrawCorners(dl, x, topY, w, h, col); break;
                case BoxStyle::Full2D:   dl->AddRect(ImVec2(x, topY), ImVec2(x + w, topY + h), col, 0, 0, 1.4f); break;
                case BoxStyle::Rounded:  dl->AddRect(ImVec2(x, topY), ImVec2(x + w, topY + h), col, 4.0f, 0, 1.4f); break;
                case BoxStyle::Edgeless: /* no box, label only */ break;
            }

            if (g_cfg.snapline) {
                float sx = (float)Overlay::Width() * 0.5f;
                float sy = (float)Overlay::Height();
                dl->AddLine(ImVec2(sx, sy), ImVec2(cx, botY), col, 1.0f);
            }

            if (g_cfg.healthBar) DrawHealthBar(dl, x, topY, h, p.health, p.maxHealth);

            float labelY = topY - 18.0f;
            if (g_cfg.nameTag) {
                char nameBuf[96];
                if (g_cfg.clanTag && !p.clan.empty())
                    std::snprintf(nameBuf, 96, "[%s] %s", p.clan.c_str(), p.name.c_str());
                else
                    std::snprintf(nameBuf, 96, "%s", p.name.c_str());
                ImVec2 sz = ImGui::CalcTextSize(nameBuf);
                DrawShadowText(dl, ImVec2(cx - sz.x * 0.5f, labelY), g_cfg.colorText, nameBuf);
                labelY -= 14.0f;
            }
            if (g_cfg.distanceTag) {
                char d[16]; std::snprintf(d, 16, "%dm", (int)p.distance);
                ImVec2 sz = ImGui::CalcTextSize(d);
                DrawShadowText(dl, ImVec2(cx - sz.x * 0.5f, topY + h + 2.0f), g_cfg.colorText, d);
            }
            float sideY = topY;
            if (g_cfg.roleTag && !p.role.empty()) {
                DrawShadowText(dl, ImVec2(x + w + 4.0f, sideY), g_cfg.colorText, p.role.c_str());
                sideY += 14.0f;
            }
            if (g_cfg.heldItemTag && !p.heldItem.empty()) {
                DrawShadowText(dl, ImVec2(x + w + 4.0f, sideY), g_cfg.colorText, p.heldItem.c_str());
                sideY += 14.0f;
            }

            if (g_cfg.portrait) {
                Portrait::Draw(p.name.empty() ? "default" : p.name, x, topY - 22.0f, 20.0f, 20.0f);
            }

            if (g_cfg.skeleton)  DrawSkeleton (dl, p, col);
            if (g_cfg.chamsWire) DrawChamsWire(dl, p, col);

            if (g_cfg.headDot) {
                dl->AddCircleFilled(ImVec2(headS[0], headS[1]), 2.0f, IM_COL32(255, 255, 255, 230), 10);
                dl->AddCircle      (ImVec2(headS[0], headS[1]), 3.0f, IM_COL32(0, 0, 0, 220), 10, 1.0f);
            }
        } else if (g_cfg.showOffscreen) {
            far_++;
            uint32_t col = p.alive ? g_cfg.colorEnemy : g_cfg.colorDead;
            DrawFar(dl, p.head, col);
        }
    }

    if (g_cfg.lootEsp) {
        for (auto& li : loot) {
            if (li.rarity < g_cfg.lootMinRarity) continue;
            if (li.distance > g_cfg.lootMaxDistance) continue;
            if (!AppState::LootPasses(li.name, li.rarity)) continue;
            DrawLootItem(dl, li);
        }
    }
    for (auto& m : marks) {
        if (m.kind == WorldMark::Door && !g_cfg.doorEsp) continue;
        if (m.kind != WorldMark::Door && !g_cfg.vehicleEsp) continue;
        DrawMark(dl, m);
    }

    if (g_cfg.radar) DrawRadar(dl, players, loot);

    static int diagTick = 0;
    if ((++diagTick % 120) == 0)
        Console::Log("ESP: vis=%d far=%d loot=%zu marks=%zu", vis, far_, loot.size(), marks.size());
}
