#include "game.h"
#include "mem.h"
#include "offsets.h"
#include "console.h"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cmath>
#include <cstring>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "psapi.lib")

// ==========================================================================
// Globals
// ==========================================================================
namespace {

using Clock = std::chrono::steady_clock;

uintptr_t g_gameAsm   = 0;
uintptr_t g_unityBase = 0;

uintptr_t g_battleModel    = 0;
uintptr_t g_battleEntities = 0;
uintptr_t g_worldEntities  = 0;
uintptr_t g_viewUpdater    = 0;
uintptr_t g_cineBrain      = 0;

HWND  g_hwnd    = nullptr;
int   g_scrW    = 0;
int   g_scrH    = 0;
char  g_procName[64] = "ROB RUN.exe";

Cam   g_cam;
Cam   g_heldCam;
int   g_camHoldFrames = 0;

std::vector<Player>    g_players;
std::vector<LootItem>  g_loot;
std::vector<WorldMark> g_marks;
std::mutex             g_playerMutex;

std::unordered_map<uint64_t, Vec3>   g_posMem;
std::unordered_map<std::string, std::string> g_roleMap;

// klass pointers (resolved)
struct Klasses {
    uintptr_t battleModel = 0;
    uintptr_t battleEntitiesModel = 0;
    uintptr_t battleCharacterModel = 0;
    uintptr_t battleCharacterClientModel = 0;
    uintptr_t battleCameraModel = 0;
    uintptr_t worldEntitiesModel = 0;
    uintptr_t randomLootObjectModel = 0;
    uintptr_t lootNodeModel = 0;
    uintptr_t radiusInteractEntityModel = 0;
    uintptr_t doorModel = 0;
    uintptr_t aimTargetUpdater = 0;
    uintptr_t entitiesUpdater = 0;
    uintptr_t cinemachineBrain = 0;
    uintptr_t weaponSpread = 0;
    uintptr_t spriteRecoilView = 0;
    uintptr_t weaponInertiaData = 0;
};
Klasses g_k;

// scanner thread
std::thread       g_scanThread;
std::atomic<bool> g_scanRunning{false};
std::atomic<bool> g_rootsValid{false};

// teleport state
struct TpState {
    Game::TpKind kind = Game::TpKind::None;
    uint32_t entityId = 0;
    Vec3     point;
    Vec3     flyVel;
    bool     fly = false;
    bool     active = false;
};
TpState g_tp;

// recoil/spread/sway pin-restore
std::unordered_map<uintptr_t, uint32_t> g_pinRestore;
Game::GunToggles                        g_gunState;

// sensitivity calibration
struct SensSample { double dxRot, dyRot, mx, my; };
std::vector<SensSample> g_sensSamples;
float g_cpdX = 0.0f, g_cpdY = 0.0f;
float g_lastYaw = 0.0f, g_lastPitch = 0.0f;
bool  g_sensHaveLast = false;
float g_lastAimDx = 0.0f, g_lastAimDy = 0.0f;

// gather pacing
auto g_lastCharGather = Clock::now() - std::chrono::seconds(1);
auto g_lastLootGather = Clock::now() - std::chrono::seconds(1);

// ==========================================================================
// Math helpers
// ==========================================================================
inline bool IsFiniteVec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool InWorld(const Vec3& v) {
    return IsFiniteVec(v) &&
           std::fabs(v.x) < offsets::world_bound &&
           std::fabs(v.y) < offsets::world_bound &&
           std::fabs(v.z) < offsets::world_bound &&
           v.y > offsets::parked_slot_y;
}

inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

inline float Length(const Vec3& a) { return std::sqrt(Dot(a,a)); }

inline bool Unitish(const Vec3& a) {
    float l = Length(a);
    return l > 0.5f && l < 1.5f;
}

inline Vec3 Normalize(const Vec3& a) {
    float l = Length(a);
    if (l < 1e-6f) return Vec3(0,0,1);
    return Vec3(a.x/l, a.y/l, a.z/l);
}

inline Vec3 Sub(const Vec3& a, const Vec3& b) { return Vec3(a.x-b.x, a.y-b.y, a.z-b.z); }
inline Vec3 Add(const Vec3& a, const Vec3& b) { return Vec3(a.x+b.x, a.y+b.y, a.z+b.z); }
inline Vec3 Mul(const Vec3& a, float s)       { return Vec3(a.x*s, a.y*s, a.z*s); }

// quaternion xyzw * vec3
inline Vec3 QRotate(const float q[4], const Vec3& v) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    Vec3 u(x, y, z);
    float s = w;
    Vec3 uv = Vec3(u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x);
    Vec3 uuv= Vec3(u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x);
    return Vec3(v.x + 2.0f*(s*uv.x + uuv.x),
                v.y + 2.0f*(s*uv.y + uuv.y),
                v.z + 2.0f*(s*uv.z + uuv.z));
}

// ==========================================================================
// Window
// ==========================================================================
struct FindWndCtx { DWORD pid; HWND hwnd; };

BOOL CALLBACK FindWndProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindWndCtx*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p == c->pid) {
        wchar_t title[128] = {};
        GetWindowTextW(h, title, 127);
        if (IsWindowVisible(h) && title[0] && GetWindow(h, GW_OWNER) == nullptr) {
            c->hwnd = h;
            return FALSE;
        }
    }
    return TRUE;
}

void RefreshWindowImpl() {
    FindWndCtx c{ Mem::Pid(), nullptr };
    EnumWindows(FindWndProc, (LPARAM)&c);
    if (!c.hwnd) return;
    g_hwnd = c.hwnd;
    RECT rc;
    if (GetClientRect(g_hwnd, &rc)) {
        g_scrW = rc.right - rc.left;
        g_scrH = rc.bottom - rc.top;
    }
}

// ==========================================================================
// FindProcess
// ==========================================================================
DWORD Game_FindProcessByName(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    char cvt[260];
    if (Process32FirstW(snap, &pe)) {
        do {
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, cvt, sizeof(cvt), nullptr, nullptr);
            if (_stricmp(cvt, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// ==========================================================================
// Klass validation / name scan
// ==========================================================================
bool ValidateKlass(uintptr_t k) {
    if (!Mem::IsValidPtr(k)) return false;
    uintptr_t elem = Mem::ReadPtr(k + offsets::klass_element_class);
    uintptr_t cast = Mem::ReadPtr(k + offsets::klass_cast_class);
    return elem == k && cast == k;
}

std::string ReadKlassName(uintptr_t k) {
    uintptr_t np = Mem::ReadPtr(k + offsets::klass_name);
    if (!Mem::IsValidPtr(np)) return {};
    return Mem::ReadNativeName(np, 96);
}

bool ClassNameIs(uintptr_t klass, const char* needle) {
    std::string n = ReadKlassName(klass);
    if (n.empty()) return false;
    return n.find(needle) != std::string::npos;
}

void ScanKlassNames(const std::vector<std::string>& wanted,
                    std::unordered_map<std::string, uintptr_t>& out)
{
    out.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Mem::Pid());
    if (!h) return;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = offsets::klass_arena_lo;
    while (addr < offsets::klass_arena_hi && out.size() < wanted.size()) {
        if (!VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) &&
            !(mbi.Protect & PAGE_GUARD) && (mbi.Type & MEM_PRIVATE)) {
            size_t sz = (size_t)mbi.RegionSize;
            if (sz > 0x100000) sz = 0x100000; // cap per region
            std::vector<uint8_t> buf(sz);
            SIZE_T got = 0;
            if (ReadProcessMemory(h, mbi.BaseAddress, buf.data(), sz, &got) && got > 0x100) {
                uintptr_t base = (uintptr_t)mbi.BaseAddress;
                for (size_t off = 0; off + 0xC0 < got; off += 8) {
                    uintptr_t self = base + off;
                    uintptr_t np = *(uintptr_t*)(buf.data() + off + offsets::klass_name);
                    if (!Mem::IsValidPtr(np)) continue;
                    std::string nm = Mem::ReadNativeName(np, 80);
                    if (nm.empty()) continue;
                    for (const auto& w : wanted) {
                        if (nm == w && out.find(w) == out.end()) {
                            if (ValidateKlass(self)) out[w] = self;
                            break;
                        }
                    }
                    if (out.size() == wanted.size()) break;
                }
            }
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (addr <= (uintptr_t)mbi.BaseAddress) break;
    }
    CloseHandle(h);
}

uintptr_t ResolveKlassByRva(uintptr_t rva) {
    if (!g_gameAsm || !rva) return 0;
    uintptr_t ti = Mem::ReadPtr(g_gameAsm + rva);
    if (!Mem::IsValidPtr(ti)) return 0;
    uintptr_t k = Mem::ReadPtr(ti);
    if (!Mem::IsValidPtr(k)) return 0;
    return ValidateKlass(k) ? k : 0;
}

void ResolveAllKlasses() {
    for (const auto& s : offsets::type_info_sets) {
        Klasses c;
        c.battleModel                = ResolveKlassByRva(s.battle_model);
        c.battleEntitiesModel        = ResolveKlassByRva(s.battle_entities_model);
        c.battleCharacterModel       = ResolveKlassByRva(s.battle_character_model);
        c.battleCharacterClientModel = ResolveKlassByRva(s.battle_character_client_model);
        c.battleCameraModel          = ResolveKlassByRva(s.battle_camera_model);
        c.worldEntitiesModel         = ResolveKlassByRva(s.world_entities_model);
        c.randomLootObjectModel      = ResolveKlassByRva(s.random_loot_object_model);
        c.lootNodeModel              = ResolveKlassByRva(s.loot_node_model);
        c.radiusInteractEntityModel  = ResolveKlassByRva(s.radius_interact_entity_model);
        c.doorModel                  = ResolveKlassByRva(s.door_model);
        c.aimTargetUpdater           = ResolveKlassByRva(s.aim_target_updater);
        c.entitiesUpdater            = ResolveKlassByRva(s.entities_updater);
        c.cinemachineBrain           = ResolveKlassByRva(s.cinemachine_brain);
        c.weaponSpread               = ResolveKlassByRva(s.weapon_spread);
        c.spriteRecoilView           = ResolveKlassByRva(s.sprite_recoil_view);
        c.weaponInertiaData          = ResolveKlassByRva(s.weapon_inertia_data);
        int hits = 0;
        uintptr_t* p = (uintptr_t*)&c;
        for (size_t i = 0; i < sizeof(Klasses)/sizeof(uintptr_t); ++i) if (p[i]) hits++;
        if (hits >= 4) { g_k = c; Console::Log("klass RVA set hit (%d/%zu)", hits, sizeof(Klasses)/sizeof(uintptr_t)); break; }
    }
    // Fill the rest by name-scan
    std::vector<std::string> wanted;
    auto need = [&](uintptr_t& dst, const char* n) {
        if (!dst) wanted.push_back(n);
    };
    need(g_k.battleModel, "BattleModel");
    need(g_k.battleEntitiesModel, "BattleEntitiesModel");
    need(g_k.battleCharacterModel, "BattleCharacterModel");
    need(g_k.battleCharacterClientModel, "BattleCharacterClientModel");
    need(g_k.battleCameraModel, "BattleCameraModel");
    need(g_k.worldEntitiesModel, "WorldEntitiesModel");
    need(g_k.randomLootObjectModel, "RandomLootObjectModel");
    need(g_k.lootNodeModel, "LootNodeModel");
    need(g_k.radiusInteractEntityModel, "RadiusInteractEntityModel");
    need(g_k.doorModel, "DoorModel");
    need(g_k.aimTargetUpdater, "BattleAimTargetUpdater");
    need(g_k.entitiesUpdater, "BattleEntitiesUpdater");
    need(g_k.cinemachineBrain, "CinemachineBrain");
    need(g_k.weaponSpread, "WeaponSpread");
    need(g_k.spriteRecoilView, "SpriteRecoilViewDescription");
    need(g_k.weaponInertiaData, "WeaponInertiaData");
    if (wanted.empty()) return;
    std::unordered_map<std::string, uintptr_t> hits;
    ScanKlassNames(wanted, hits);
    auto fill = [&](uintptr_t& dst, const char* n) {
        auto it = hits.find(n);
        if (it != hits.end()) dst = it->second;
    };
    fill(g_k.battleModel, "BattleModel");
    fill(g_k.battleEntitiesModel, "BattleEntitiesModel");
    fill(g_k.battleCharacterModel, "BattleCharacterModel");
    fill(g_k.battleCharacterClientModel, "BattleCharacterClientModel");
    fill(g_k.battleCameraModel, "BattleCameraModel");
    fill(g_k.worldEntitiesModel, "WorldEntitiesModel");
    fill(g_k.randomLootObjectModel, "RandomLootObjectModel");
    fill(g_k.lootNodeModel, "LootNodeModel");
    fill(g_k.radiusInteractEntityModel, "RadiusInteractEntityModel");
    fill(g_k.doorModel, "DoorModel");
    fill(g_k.aimTargetUpdater, "BattleAimTargetUpdater");
    fill(g_k.entitiesUpdater, "BattleEntitiesUpdater");
    fill(g_k.cinemachineBrain, "CinemachineBrain");
    fill(g_k.weaponSpread, "WeaponSpread");
    fill(g_k.spriteRecoilView, "SpriteRecoilViewDescription");
    fill(g_k.weaponInertiaData, "WeaponInertiaData");
}

// ==========================================================================
// Camera reads (raw)
// ==========================================================================
Vec3 ReadVec3(uintptr_t addr) {
    Vec3 v{};
    Mem::ReadRaw(Mem::Pid(), addr, &v, sizeof(float)*3);
    if (!IsFiniteVec(v)) return Vec3();
    return v;
}

bool ValidateBattleModel(uintptr_t bm) {
    if (!Mem::IsValidPtr(bm)) return false;
    uintptr_t cam = Mem::ReadPtr(bm + offsets::battle_model::camera);
    if (!Mem::IsValidPtr(cam)) return false;
    Vec3 fwd = ReadVec3(cam + offsets::battle_camera_model::forward);
    Vec3 rgt = ReadVec3(cam + offsets::battle_camera_model::right);
    Vec3 up  = ReadVec3(cam + offsets::battle_camera_model::up);
    return Unitish(fwd) && Unitish(rgt) && Unitish(up);
}

// ==========================================================================
// Root discovery: three strategies
// ==========================================================================
bool TryStaticFieldPath() {
    if (!g_k.battleEntitiesModel) return false;
    uintptr_t sf = Mem::ReadPtr(g_k.battleEntitiesModel + offsets::klass_static_fields);
    if (!Mem::IsValidPtr(sf)) return false;
    uintptr_t inst = Mem::ReadPtr(sf + 0x08);
    if (!Mem::IsValidPtr(inst)) return false;
    uintptr_t bm = Mem::ReadPtr(inst + 0x10);
    if (!ValidateBattleModel(bm)) return false;
    g_battleModel    = bm;
    g_battleEntities = inst;
    return true;
}

bool TryGomPath() {
    if (!g_unityBase) return false;
    uintptr_t gom = Mem::ReadPtr(g_unityBase + offsets::gom_rva);
    if (!Mem::IsValidPtr(gom)) return false;
    uintptr_t head = Mem::ReadPtr(gom);
    uintptr_t node = head;
    int steps = 0;
    while (Mem::IsValidPtr(node) && steps < 2048) {
        uintptr_t native = Mem::ReadPtr(node + offsets::gom_native_ptr);
        if (Mem::IsValidPtr(native)) {
            uintptr_t klass = Mem::ReadPtr(native);
            if (klass == g_k.battleModel && ValidateBattleModel(native)) {
                g_battleModel = native;
                g_battleEntities = Mem::ReadPtr(native + offsets::battle_model::entities);
                return true;
            }
        }
        uintptr_t next = Mem::ReadPtr(node + 0x08);
        if (next == head || !Mem::IsValidPtr(next)) break;
        node = next;
        ++steps;
    }
    return false;
}

bool TryHeapScan() {
    if (!g_k.battleEntitiesModel) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Mem::Pid());
    if (!h) return false;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    bool found = false;
    while (!found && addr < 0x7FFFFFFFFFFFULL) {
        if (!VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && (mbi.Type & MEM_PRIVATE) &&
            !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD)) {
            size_t sz = (size_t)mbi.RegionSize;
            if (sz > 0x200000) sz = 0x200000;
            std::vector<uintptr_t> buf(sz / sizeof(uintptr_t));
            SIZE_T got = 0;
            if (ReadProcessMemory(h, mbi.BaseAddress, buf.data(), sz, &got)) {
                size_t n = got / sizeof(uintptr_t);
                for (size_t i = 0; i < n; ++i) {
                    if (buf[i] != g_k.battleEntitiesModel) continue;
                    uintptr_t cand = (uintptr_t)mbi.BaseAddress + i * sizeof(uintptr_t);
                    uintptr_t bm = Mem::ReadPtr(cand + 0x10);
                    if (!ValidateBattleModel(bm)) continue;
                    g_battleModel    = bm;
                    g_battleEntities = cand;
                    found = true;
                    break;
                }
            }
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    CloseHandle(h);
    return found;
}

void FindViewUpdater() {
    if (g_viewUpdater && g_cineBrain) return;
    if (!g_k.aimTargetUpdater) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Mem::Pid());
    if (!h) return;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    while ((!g_viewUpdater || !g_cineBrain) && addr < 0x7FFFFFFFFFFFULL) {
        if (!VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && (mbi.Type & MEM_PRIVATE) &&
            !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD)) {
            size_t sz = (size_t)mbi.RegionSize;
            if (sz > 0x200000) sz = 0x200000;
            std::vector<uintptr_t> buf(sz / sizeof(uintptr_t));
            SIZE_T got = 0;
            if (ReadProcessMemory(h, mbi.BaseAddress, buf.data(), sz, &got)) {
                size_t n = got / sizeof(uintptr_t);
                for (size_t i = 0; i < n; ++i) {
                    uintptr_t v = buf[i];
                    if (v == g_k.aimTargetUpdater) {
                        uintptr_t obj = (uintptr_t)mbi.BaseAddress + i*sizeof(uintptr_t);
                        uintptr_t br  = Mem::ReadPtr(obj + offsets::aim_target_updater::brain);
                        if (Mem::IsValidPtr(br)) { g_cineBrain = br; }
                    }
                    if (v == g_k.entitiesUpdater) {
                        g_viewUpdater = (uintptr_t)mbi.BaseAddress + i*sizeof(uintptr_t);
                    }
                }
            }
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    CloseHandle(h);
}

// ==========================================================================
// Camera update
// ==========================================================================
void UpdateCameraImpl() {
    if (!g_battleModel) { g_camHoldFrames++; return; }
    uintptr_t cam = Mem::ReadPtr(g_battleModel + offsets::battle_model::camera);
    if (!Mem::IsValidPtr(cam)) { g_camHoldFrames++; return; }

    Cam c;
    c.pos   = ReadVec3(cam + offsets::battle_camera_model::position);
    c.fwd   = ReadVec3(cam + offsets::battle_camera_model::forward);
    c.right = ReadVec3(cam + offsets::battle_camera_model::right);
    c.up    = ReadVec3(cam + offsets::battle_camera_model::up);
    if (!Unitish(c.fwd) || !Unitish(c.right) || !Unitish(c.up)) {
        g_camHoldFrames++;
        return;
    }
    c.fov = offsets::camera_fov_degrees;
    if (g_cineBrain) {
        float fv = 0;
        if (Mem::ReadRaw(Mem::Pid(), g_cineBrain + offsets::cinemachine_brain::fov, &fv, 4) &&
            fv > 20.0f && fv < 170.0f) c.fov = fv;
    }
    c.valid = true;
    g_cam = c;
    g_heldCam = c;
    g_camHoldFrames = 0;
    Game::ObserveAimTurn(c.fwd);
}

Cam GetCameraForUse() {
    if (g_cam.valid && g_camHoldFrames == 0) return g_cam;
    if (g_camHoldFrames < 12 && g_heldCam.valid) return g_heldCam;
    return g_cam;
}

// ==========================================================================
// Projection
// ==========================================================================
bool ProjectImpl(const Vec3& pos, float out[2], float* outDepth, bool cull) {
    Cam c = GetCameraForUse();
    if (!c.valid || g_scrW <= 0 || g_scrH <= 0) return false;
    float dxw = pos.x - c.pos.x;
    float dyw = pos.y - c.pos.y;
    float dzw = pos.z - c.pos.z;
    float depth = dxw*c.fwd.x + dyw*c.fwd.y + dzw*c.fwd.z;
    float dx    = dxw*c.right.x + dyw*c.right.y + dzw*c.right.z;
    float dy    = dxw*c.up.x    + dyw*c.up.y    + dzw*c.up.z;
    if (outDepth) *outDepth = depth;
    if (cull && depth < 0.15f) return false;
    if (depth < 0.05f) { dx = -dx; dy = -dy; depth = 0.05f; }
    float tanHalf = std::tan(c.fov * 0.5f * 3.14159265358979323846f / 180.0f);
    float aspect  = (float)g_scrW / (float)g_scrH;
    float nx = (dx / depth) / (tanHalf * aspect);
    float ny = (dy / depth) / tanHalf;
    out[0] = ((float)g_scrW * 0.5f) * (1.0f + nx);
    out[1] = ((float)g_scrH * 0.5f) * (1.0f - ny);
    if (cull) {
        if (out[0] < -1000.0f || out[0] > g_scrW + 1000.0f) return false;
        if (out[1] < -1000.0f || out[1] > g_scrH + 1000.0f) return false;
    }
    return true;
}

// ==========================================================================
// Dictionary walk (IL2CPP Dictionary<int,T>)
// ==========================================================================
void IterateDict(uintptr_t dict, std::vector<std::pair<int32_t, uintptr_t>>& out, size_t cap = 2048) {
    out.clear();
    if (!Mem::IsValidPtr(dict)) return;
    uintptr_t entries = Mem::ReadPtr(dict + offsets::dict_entries);
    int32_t count = Mem::ReadT<int32_t>(dict + offsets::dict_count);
    if (!Mem::IsValidPtr(entries) || count <= 0 || count > (int32_t)cap) return;
    uintptr_t base = entries + offsets::array_items;
    for (int32_t i = 0; i < count; ++i) {
        uintptr_t e = base + (uintptr_t)i * offsets::dict_entry_stride;
        int32_t hash = Mem::ReadT<int32_t>(e + offsets::dict_entry_hash);
        if (hash < 0) continue;
        int32_t key  = Mem::ReadT<int32_t>(e + offsets::dict_entry_key);
        uintptr_t val = Mem::ReadPtr(e + offsets::dict_entry_value);
        if (!Mem::IsValidPtr(val)) continue;
        out.emplace_back(key, val);
    }
}

// ==========================================================================
// Position reading
// ==========================================================================
bool ReadPackedVec3(uintptr_t addr, Vec3& out) {
    if (!Mem::IsValidPtr(addr)) return false;
    Vec3 v = ReadVec3(addr);
    if (!InWorld(v)) return false;
    out = v;
    return true;
}

bool ReadBoxedVec3(uintptr_t transform, Vec3& out) {
    if (!Mem::IsValidPtr(transform)) return false;
    // packed first
    Vec3 v = ReadVec3(transform + offsets::transform_packed::vec3_packed);
    if (InWorld(v)) { out = v; return true; }
    // three primitive pointers
    uintptr_t px = Mem::ReadPtr(transform + offsets::transform_packed::prim_x);
    uintptr_t py = Mem::ReadPtr(transform + offsets::transform_packed::prim_y);
    uintptr_t pz = Mem::ReadPtr(transform + offsets::transform_packed::prim_z);
    if (!Mem::IsValidPtr(px) || !Mem::IsValidPtr(py) || !Mem::IsValidPtr(pz)) return false;
    float x = Mem::ReadT<float>(px + offsets::primitive::value);
    float y = Mem::ReadT<float>(py + offsets::primitive::value);
    float z = Mem::ReadT<float>(pz + offsets::primitive::value);
    Vec3 r(x, y, z);
    if (!InWorld(r)) return false;
    out = r;
    return true;
}

bool ReadPosition(uintptr_t model, Vec3& out) {
    // 1. client transform
    uintptr_t t = Mem::ReadPtr(model + offsets::battle_character_model::client_transform);
    if (ReadBoxedVec3(t, out)) return true;
    // 2. server transform
    t = Mem::ReadPtr(model + offsets::battle_character_model::server_transform);
    if (ReadBoxedVec3(t, out)) return true;
    // 3. replicated transform
    t = Mem::ReadPtr(model + offsets::battle_character_model::replicated_tfm);
    if (Mem::IsValidPtr(t)) {
        uintptr_t p = Mem::ReadPtr(t + offsets::replicated_transform::position_model);
        if (ReadPackedVec3(p + offsets::replicated_transform::position_vec3, out)) return true;
    }
    // 4. common data
    uintptr_t data = Mem::ReadPtr(model + offsets::battle_character_model::data);
    if (Mem::IsValidPtr(data)) {
        uintptr_t cd = Mem::ReadPtr(data + offsets::data_chain::common_data);
        if (ReadPackedVec3(cd + offsets::common_data::position_packed, out)) return true;
    }
    // 5. center position
    if (ReadPackedVec3(model + offsets::battle_character_model::center_position, out)) return true;
    // 6. cache
    auto it = g_posMem.find((uint64_t)model);
    if (it != g_posMem.end()) { out = it->second; return true; }
    return false;
}

// ==========================================================================
// Transform hierarchy walk
// ==========================================================================
struct TrsLocal { Vec3 pos; float q[4]; Vec3 scl; };

bool ReadTrs(uintptr_t matrices, int32_t index, TrsLocal& out) {
    if (!Mem::IsValidPtr(matrices) || index < 0) return false;
    uintptr_t addr = matrices + (uintptr_t)index * offsets::transform_hier::trs_stride;
    if (!Mem::ReadRaw(Mem::Pid(), addr + offsets::transform_hier::trs_pos,  &out.pos, 12)) return false;
    if (!Mem::ReadRaw(Mem::Pid(), addr + offsets::transform_hier::trs_quat, out.q,    16)) return false;
    if (!Mem::ReadRaw(Mem::Pid(), addr + offsets::transform_hier::trs_scl,  &out.scl, 12)) return false;
    return true;
}

Vec3 WorldIndex(uintptr_t hierarchy, int32_t idx) {
    uintptr_t mats    = Mem::ReadPtr(hierarchy + offsets::transform_hier::matrices);
    uintptr_t parents = Mem::ReadPtr(hierarchy + offsets::transform_hier::parents);
    Vec3 acc(0,0,0);
    Vec3 scl(1,1,1);
    float qacc[4] = { 0, 0, 0, 1 };
    int steps = 0;
    while (idx >= 0 && steps < 24) {
        TrsLocal t;
        if (!ReadTrs(mats, idx, t)) break;
        // local point in parent space: pos * parent scale, rotated by parent q, add parent pos; iterative
        Vec3 scaled(t.pos.x * scl.x, t.pos.y * scl.y, t.pos.z * scl.z);
        Vec3 rotated = QRotate(qacc, scaled);
        acc = Add(acc, rotated);
        // compose q: qacc = qacc * t.q (xyzw); accumulate scale multiplicatively
        float ax = qacc[0], ay = qacc[1], az = qacc[2], aw = qacc[3];
        float bx = t.q[0],  by = t.q[1],  bz = t.q[2],  bw = t.q[3];
        qacc[0] = aw*bx + ax*bw + ay*bz - az*by;
        qacc[1] = aw*by - ax*bz + ay*bw + az*bx;
        qacc[2] = aw*bz + ax*by - ay*bx + az*bw;
        qacc[3] = aw*bw - ax*bx - ay*by - az*bz;
        scl = Vec3(scl.x * t.scl.x, scl.y * t.scl.y, scl.z * t.scl.z);
        int32_t next = Mem::ReadT<int32_t>(parents + (uintptr_t)idx * 4);
        if (next == idx) break;
        idx = next;
        ++steps;
    }
    return acc;
}

Vec3 WorldFromTransform(uintptr_t managedTfm) {
    if (!Mem::IsValidPtr(managedTfm)) return Vec3();
    uintptr_t native = Mem::ReadPtr(managedTfm + offsets::unity_transform::native_ptr);
    if (!Mem::IsValidPtr(native)) return Vec3();
    uintptr_t hier = Mem::ReadPtr(native + offsets::unity_transform::hierarchy_ptr);
    int32_t idx   = Mem::ReadT<int32_t>(native + offsets::unity_transform::hier_index);
    if (!Mem::IsValidPtr(hier) || idx < 0) return Vec3();
    return WorldIndex(hier, idx);
}

// ==========================================================================
// Health
// ==========================================================================
void ReadHealth(uintptr_t model, bool local, int32_t& hp, int32_t& maxHp) {
    if (local) {
        uintptr_t pm = Mem::ReadPtr(model + offsets::battle_character_model::private_model);
        if (!Mem::IsValidPtr(pm)) return;
        uintptr_t hm = Mem::ReadPtr(pm + offsets::private_model::health_model);
        if (!Mem::IsValidPtr(hm)) return;
        uintptr_t vm = Mem::ReadPtr(hm + 0x10);
        if (!Mem::IsValidPtr(vm)) return;
        float v = Mem::ReadT<float>(vm + offsets::health_value_model::value_float);
        if (std::isfinite(v) && v >= 0.0f && v <= 10000.0f) hp = (int32_t)v;
        maxHp = 100;
    } else {
        uintptr_t data = Mem::ReadPtr(model + offsets::battle_character_model::data);
        if (!Mem::IsValidPtr(data)) return;
        uintptr_t pd = Mem::ReadPtr(data + offsets::data_chain::private_data);
        if (!Mem::IsValidPtr(pd)) return;
        uintptr_t h  = Mem::ReadPtr(pd + offsets::private_data::health);
        if (!Mem::IsValidPtr(h)) return;
        int32_t v = Mem::ReadT<int32_t>(h + offsets::health_model::value_int32);
        if (v >= 0 && v <= 10000) hp = v;
        maxHp = 100;
    }
}

// ==========================================================================
// Slot names
// ==========================================================================
void CollectSlotNames(std::unordered_map<int32_t, std::pair<std::string, std::string>>& out) {
    if (!g_battleEntities) return;
    uintptr_t slots = Mem::ReadPtr(g_battleEntities + offsets::battle_entities_model::character_slots);
    if (!Mem::IsValidPtr(slots)) return;
    std::vector<std::pair<int32_t, uintptr_t>> entries;
    IterateDict(slots, entries, 256);
    for (auto& e : entries) {
        uintptr_t cs = Mem::ReadPtr(e.second + offsets::char_slot::client_slots);
        if (!Mem::IsValidPtr(cs)) continue;
        uintptr_t nickArr = Mem::ReadPtr(cs + offsets::char_slot::nickname_array);
        std::string nick;
        if (Mem::IsValidPtr(nickArr)) {
            uintptr_t s = Mem::ReadPtr(nickArr + offsets::array_items);
            nick = Mem::ReadIl2CppString(s);
        }
        uintptr_t clanStr = Mem::ReadPtr(cs + offsets::char_slot::clan_tag);
        std::string clan = Mem::ReadIl2CppString(clanStr);
        out[e.first] = { nick, clan };
    }
}

// ==========================================================================
// Hitbox bones (fallback head/foot)
// ==========================================================================
void ReadHitboxBones(uintptr_t model, Vec3 root, Vec3& head, Vec3& foot) {
    head = Vec3(root.x, root.y + offsets::skeleton::head_height, root.z);
    foot = Vec3(root.x, root.y + offsets::skeleton::foot_height, root.z);
    uintptr_t hit = Mem::ReadPtr(model + offsets::battle_character_model::hitbox_access);
    if (!Mem::IsValidPtr(hit)) return;
    uintptr_t sh  = Mem::ReadPtr(hit + offsets::hitbox::shapes);
    if (!Mem::IsValidPtr(sh)) return;
    uintptr_t node= Mem::ReadPtr(sh + offsets::hitbox::node);
    if (!Mem::IsValidPtr(node)) return;
    Vec3 c = ReadVec3(node + offsets::hitbox::cap_center);
    Vec3 t = ReadVec3(node + offsets::hitbox::cap_top);
    if (InWorld(c) && InWorld(t)) {
        // whichever has higher y is head
        if (t.y > c.y) { head = t; foot = Vec3(c.x, 2*c.y - t.y, c.z); }
        else           { head = c; foot = Vec3(t.x, 2*t.y - c.y, t.z); }
    }
}

// ==========================================================================
// Rig fill from view
// ==========================================================================
const char* kJointNames[16] = {
    "M_spine_1_jnt","M_spine_3_jnt","M_neck_jnt","M_head_1_jnt",
    "L_arm_0_jnt","R_arm_0_jnt","L_arm_2_jnt","R_arm_2_jnt",
    "L_arm_3_jnt","R_arm_3_jnt","L_leg_1_jnt","R_leg_1_jnt",
    "L_leg_2_jnt","R_leg_2_jnt","L_foot_1_jnt","R_foot_1_jnt",
};

void FillRig(uintptr_t view, BoneRig& rig, Vec3 root) {
    rig.filled = false;
    for (auto& j : rig.joints) j = root;
    if (!Mem::IsValidPtr(view)) return;
    uintptr_t headT = Mem::ReadPtr(view + offsets::char_view::head_position);
    if (Mem::IsValidPtr(headT)) {
        Vec3 h = WorldFromTransform(headT);
        if (InWorld(h)) rig.joints[3] = h;
    }
    // without a managed bone map we fall back to synthesized heights anchored at root
    rig.joints[0]  = Vec3(root.x, root.y + 0.9f,  root.z);
    rig.joints[1]  = Vec3(root.x, root.y + 1.35f, root.z);
    rig.joints[2]  = Vec3(root.x, root.y + 1.50f, root.z);
    if (rig.joints[3].y < root.y + 1.0f) rig.joints[3] = Vec3(root.x, root.y + offsets::skeleton::head_height, root.z);
    rig.joints[4]  = Vec3(root.x - 0.20f, root.y + 1.40f, root.z);
    rig.joints[5]  = Vec3(root.x + 0.20f, root.y + 1.40f, root.z);
    rig.joints[6]  = Vec3(root.x - 0.30f, root.y + 1.15f, root.z);
    rig.joints[7]  = Vec3(root.x + 0.30f, root.y + 1.15f, root.z);
    rig.joints[8]  = Vec3(root.x - 0.32f, root.y + 0.95f, root.z);
    rig.joints[9]  = Vec3(root.x + 0.32f, root.y + 0.95f, root.z);
    rig.joints[10] = Vec3(root.x - 0.12f, root.y + 0.85f, root.z);
    rig.joints[11] = Vec3(root.x + 0.12f, root.y + 0.85f, root.z);
    rig.joints[12] = Vec3(root.x - 0.14f, root.y + 0.45f, root.z);
    rig.joints[13] = Vec3(root.x + 0.14f, root.y + 0.45f, root.z);
    rig.joints[14] = Vec3(root.x - 0.14f, root.y + 0.02f, root.z);
    rig.joints[15] = Vec3(root.x + 0.14f, root.y + 0.02f, root.z);
    rig.filled = true;
}

// ==========================================================================
// Role / held / crouch / vehicle
// ==========================================================================
std::string ProbeLootText(uintptr_t boxed) {
    if (!Mem::IsValidPtr(boxed)) return {};
    uintptr_t s = Mem::ReadPtr(boxed + 0x10);
    std::string n = Mem::ReadIl2CppString(s);
    if (!n.empty()) return n;
    return Mem::ReadIl2CppString(boxed + 0x10);
}

std::string ReadRole(uintptr_t data) {
    if (!Mem::IsValidPtr(data)) return {};
    uintptr_t cd = Mem::ReadPtr(data + offsets::data_chain::common_data);
    if (!Mem::IsValidPtr(cd)) return {};
    uintptr_t box = Mem::ReadPtr(cd + offsets::common_data::role);
    if (!Mem::IsValidPtr(box)) return {};
    uintptr_t desc = Mem::ReadPtr(box + offsets::role_desc::desc_box);
    if (!Mem::IsValidPtr(desc)) return {};
    uintptr_t str = Mem::ReadPtr(desc + offsets::role_desc::id_str);
    std::string s = Mem::ReadIl2CppString(str);
    if (s.empty()) return {};
    if (auto it = g_roleMap.find(s); it != g_roleMap.end()) return it->second;
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("cop") != std::string::npos || lower.find("police") != std::string::npos) return "Cop";
    if (lower.find("crim") != std::string::npos || lower.find("thief") != std::string::npos) return "Criminal";
    if (lower.find("civ")  != std::string::npos) return "Civilian";
    return s;
}

std::string ReadHeldItem(uintptr_t data) {
    if (!Mem::IsValidPtr(data)) return {};
    uintptr_t cd = Mem::ReadPtr(data + offsets::data_chain::common_data);
    if (!Mem::IsValidPtr(cd)) return {};
    uintptr_t box = Mem::ReadPtr(cd + offsets::common_data::item_in_hands);
    return ProbeLootText(box);
}

bool ReadCrouch(uintptr_t model) {
    uintptr_t cr = Mem::ReadPtr(model + offsets::battle_character_model::crouch);
    if (!Mem::IsValidPtr(cr)) return false;
    uintptr_t pr = Mem::ReadPtr(cr + 0x10);
    if (!Mem::IsValidPtr(pr)) return false;
    uint8_t v = Mem::ReadT<uint8_t>(pr + offsets::primitive::value);
    return v != 0;
}

bool ReadInVehicle(uintptr_t model) {
    uintptr_t iv = Mem::ReadPtr(model + offsets::battle_character_model::in_vehicle);
    if (!Mem::IsValidPtr(iv)) return false;
    uintptr_t eid= Mem::ReadPtr(iv + 0x10);
    if (!Mem::IsValidPtr(eid)) return false;
    int32_t v = Mem::ReadT<int32_t>(eid + offsets::primitive::value);
    return v >= 0 && v < 1000000;
}

// ==========================================================================
// Gather characters
// ==========================================================================
uintptr_t ResolveCharacterModel(uintptr_t value) {
    if (!Mem::IsValidPtr(value)) return 0;
    uintptr_t k = Mem::ReadPtr(value);
    if (k == g_k.battleCharacterModel) return value;
    uintptr_t inner = Mem::ReadPtr(value + offsets::player_entity_model::server_model);
    if (Mem::IsValidPtr(inner)) {
        uintptr_t k2 = Mem::ReadPtr(inner);
        if (k2 == g_k.battleCharacterModel) return inner;
    }
    return 0;
}

void BuildPlayer(int32_t entityId, uintptr_t model, bool isLocal,
                 const std::unordered_map<int32_t, std::pair<std::string,std::string>>& slots,
                 std::vector<Player>& out)
{
    Vec3 root;
    if (!ReadPosition(model, root)) return;
    g_posMem[(uint64_t)model] = root;

    Player p;
    p.model    = model;
    p.entityId = (uint32_t)entityId;
    p.root     = root;
    p.isLocalPlayer = isLocal;

    ReadHealth(model, isLocal, p.health, p.maxHealth);
    p.alive = p.health > 0;

    Vec3 head, foot;
    ReadHitboxBones(model, root, head, foot);
    p.head = head;

    uintptr_t data = Mem::ReadPtr(model + offsets::battle_character_model::data);
    p.role     = ReadRole(data);
    p.heldItem = ReadHeldItem(data);
    p.crouch   = ReadCrouch(model);
    p.inVehicle= ReadInVehicle(model);

    if (auto it = slots.find(entityId); it != slots.end()) {
        p.name = it->second.first;
        p.clan = it->second.second;
    }
    if (p.name.empty()) {
        char buf[32]; std::snprintf(buf, 32, "id%d", entityId);
        p.name = buf;
    }

    FillRig(0, p.rig, root);
    p.hasRig = p.rig.filled;

    Cam c = GetCameraForUse();
    if (c.valid) p.distance = Game::Distance3D(c.pos, root);

    p.lastSeen = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    out.push_back(std::move(p));
}

void GatherCharacters(std::vector<Player>& out) {
    out.clear();
    if (!g_battleEntities) return;

    std::unordered_map<int32_t, std::pair<std::string, std::string>> slots;
    CollectSlotNames(slots);

    // local player
    uintptr_t localEnt = Mem::ReadPtr(g_battleEntities + offsets::battle_entities_model::player);
    if (!Mem::IsValidPtr(localEnt))
        localEnt = Mem::ReadPtr(g_battleEntities + offsets::battle_entities_model::player_alt);
    uintptr_t localModel = ResolveCharacterModel(localEnt);

    std::unordered_set<uintptr_t> seen;

    // Pass 1: entity dict
    uintptr_t dict = Mem::ReadPtr(g_battleEntities + offsets::battle_entities_model::models);
    std::vector<std::pair<int32_t, uintptr_t>> entries;
    IterateDict(dict, entries);
    for (auto& e : entries) {
        uintptr_t m = ResolveCharacterModel(e.second);
        if (!m || !seen.insert(m).second) continue;
        BuildPlayer(e.first, m, m == localModel, slots, out);
    }

    // Pass 2: world characters
    if (g_worldEntities) {
        uintptr_t wdict = Mem::ReadPtr(g_worldEntities + offsets::world_entities_model::battle_characters);
        IterateDict(wdict, entries);
        for (auto& e : entries) {
            uintptr_t m = ResolveCharacterModel(e.second);
            if (!m || !seen.insert(m).second) continue;
            BuildPlayer(e.first, m, m == localModel, slots, out);
        }
    }

    // nominate local by proximity if we never saw a local flag
    Cam c = GetCameraForUse();
    if (c.valid) {
        float bestD2 = 400.0f; // 20m
        int bestIdx = -1;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].isLocalPlayer) { bestIdx = -2; break; }
            Vec3 d = Sub(out[i].root, c.pos);
            float d2 = Dot(d, d);
            if (d2 < bestD2) { bestD2 = d2; bestIdx = (int)i; }
        }
        if (bestIdx >= 0) out[bestIdx].isLocalPlayer = true;
    }
}

// ==========================================================================
// Loot
// ==========================================================================
int RarityFromText(const std::string& s) {
    if (s.empty()) return 0;
    std::string l = s;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    if (l.find("mythic") != std::string::npos)    return 6;
    if (l.find("legendary") != std::string::npos) return 5;
    if (l.find("epic") != std::string::npos)      return 4;
    if (l.find("rare") != std::string::npos)      return 3;
    if (l.find("uncommon") != std::string::npos)  return 2;
    if (l.find("common") != std::string::npos)    return 1;
    return 0;
}

std::string LearnLootName(uintptr_t obj) {
    if (!Mem::IsValidPtr(obj)) return {};
    for (uintptr_t off = 0x10; off <= 0x90; off += 0x08) {
        uintptr_t s = Mem::ReadPtr(obj + off);
        std::string n = Mem::ReadIl2CppString(s);
        if (!n.empty() && n.size() < 96) return n;
    }
    for (uintptr_t off = 0x10; off <= 0x50; off += 0x08) {
        uintptr_t inner = Mem::ReadPtr(obj + off);
        if (!Mem::IsValidPtr(inner)) continue;
        for (uintptr_t off2 = 0x10; off2 <= 0x40; off2 += 0x08) {
            uintptr_t s = Mem::ReadPtr(inner + off2);
            std::string n = Mem::ReadIl2CppString(s);
            if (!n.empty() && n.size() < 96) return n;
        }
    }
    return {};
}

void CollectLootDict(uintptr_t dict, std::vector<LootItem>& out) {
    if (!Mem::IsValidPtr(dict)) return;
    std::vector<std::pair<int32_t, uintptr_t>> entries;
    IterateDict(dict, entries);
    for (auto& e : entries) {
        uintptr_t desc = Mem::ReadPtr(e.second + offsets::loot_model::description);
        if (!Mem::IsValidPtr(desc)) desc = e.second;
        Vec3 p = ReadVec3(desc + offsets::loot_model::position);
        if (!InWorld(p)) continue;
        LootItem li;
        li.model   = e.second;
        li.pos     = p;
        li.name    = LearnLootName(desc);
        if (li.name.empty()) li.name = LearnLootName(e.second);
        li.rarity  = RarityFromText(li.name);
        Cam c = GetCameraForUse();
        if (c.valid) li.distance = Game::Distance3D(c.pos, p);
        out.push_back(std::move(li));
    }
}

void GatherLoot(std::vector<LootItem>& out) {
    out.clear();
    if (!g_worldEntities) return;
    CollectLootDict(Mem::ReadPtr(g_worldEntities + offsets::world_entities_model::random_loot), out);
    CollectLootDict(Mem::ReadPtr(g_worldEntities + offsets::world_entities_model::loot_nodes), out);
    CollectLootDict(Mem::ReadPtr(g_worldEntities + offsets::world_entities_model::radius_interacts), out);
}

void GatherMarks(std::vector<WorldMark>& out) {
    out.clear();
    if (!g_worldEntities) return;
    auto pull = [&](uintptr_t dict, WorldMark::Kind kind) {
        if (!Mem::IsValidPtr(dict)) return;
        std::vector<std::pair<int32_t, uintptr_t>> entries;
        IterateDict(dict, entries);
        for (auto& e : entries) {
            Vec3 p = ReadVec3(e.second + 0x10);
            if (!InWorld(p)) continue;
            WorldMark m;
            m.kind  = kind;
            m.model = e.second;
            m.pos   = p;
            Cam c = GetCameraForUse();
            if (c.valid) m.distance = Game::Distance3D(c.pos, p);
            out.push_back(std::move(m));
        }
    };
    pull(Mem::ReadPtr(g_worldEntities + offsets::world_entities_model::cars),       WorldMark::Vehicle);
    pull(Mem::ReadPtr(g_worldEntities + offsets::world_entities_model::helicopters),WorldMark::Helicopter);
    pull(Mem::ReadPtr(g_worldEntities + offsets::world_entities_model::doors),      WorldMark::Door);
}

// ==========================================================================
// Teleport writer
// ==========================================================================
void WriteModelPos(uintptr_t model, const Vec3& p) {
    // client transform packed
    uintptr_t t = Mem::ReadPtr(model + offsets::battle_character_model::client_transform);
    if (Mem::IsValidPtr(t)) Mem::WriteRaw(Mem::Pid(), t + offsets::transform_packed::vec3_packed, &p, sizeof(p));
    // server transform packed
    t = Mem::ReadPtr(model + offsets::battle_character_model::server_transform);
    if (Mem::IsValidPtr(t)) Mem::WriteRaw(Mem::Pid(), t + offsets::transform_packed::vec3_packed, &p, sizeof(p));
    // replicated transform -> position model vec3
    t = Mem::ReadPtr(model + offsets::battle_character_model::replicated_tfm);
    if (Mem::IsValidPtr(t)) {
        uintptr_t pm = Mem::ReadPtr(t + offsets::replicated_transform::position_model);
        if (Mem::IsValidPtr(pm))
            Mem::WriteRaw(Mem::Pid(), pm + offsets::replicated_transform::position_vec3, &p, sizeof(p));
    }
    // common data packed
    uintptr_t data = Mem::ReadPtr(model + offsets::battle_character_model::data);
    if (Mem::IsValidPtr(data)) {
        uintptr_t cd = Mem::ReadPtr(data + offsets::data_chain::common_data);
        if (Mem::IsValidPtr(cd))
            Mem::WriteRaw(Mem::Pid(), cd + offsets::common_data::position_packed, &p, sizeof(p));
    }
    // center position
    Mem::WriteRaw(Mem::Pid(), model + offsets::battle_character_model::center_position, &p, sizeof(p));
    g_posMem[(uint64_t)model] = p;
}

uintptr_t LocalModel() {
    if (!g_battleEntities) return 0;
    uintptr_t ent = Mem::ReadPtr(g_battleEntities + offsets::battle_entities_model::player);
    if (!Mem::IsValidPtr(ent)) ent = Mem::ReadPtr(g_battleEntities + offsets::battle_entities_model::player_alt);
    return ResolveCharacterModel(ent);
}

void SteerFly(Vec3& point) {
    if (!g_tp.fly) return;
    Cam c = GetCameraForUse();
    if (!c.valid) return;
    float fwd = 0, side = 0, vert = 0;
    if (GetAsyncKeyState('W') & 0x8000) fwd += 1.0f;
    if (GetAsyncKeyState('S') & 0x8000) fwd -= 1.0f;
    if (GetAsyncKeyState('D') & 0x8000) side += 1.0f;
    if (GetAsyncKeyState('A') & 0x8000) side -= 1.0f;
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) vert += 1.0f;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) vert -= 1.0f;
    float speed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0.45f : 0.20f;
    Vec3 tgt = Add(Mul(c.fwd, fwd), Add(Mul(c.right, side), Vec3(0, vert, 0)));
    tgt = Mul(tgt, speed);
    // exponential smoothing
    g_tp.flyVel.x = g_tp.flyVel.x * 0.60f + tgt.x * 0.40f;
    g_tp.flyVel.y = g_tp.flyVel.y * 0.60f + tgt.y * 0.40f;
    g_tp.flyVel.z = g_tp.flyVel.z * 0.60f + tgt.z * 0.40f;
    point = Add(point, g_tp.flyVel);
}

// ==========================================================================
// Pin-restore helper
// ==========================================================================
void PinZero(uintptr_t addr, size_t size) {
    if (!Mem::IsValidPtr(addr) || size == 0 || size > 64) return;
    uint32_t cur = 0;
    if (g_pinRestore.find(addr) == g_pinRestore.end()) {
        if (!Mem::ReadRaw(Mem::Pid(), addr, &cur, (size > 4 ? 4 : size))) return;
        g_pinRestore[addr] = cur;
    }
    uint8_t zero[64] = {};
    Mem::WriteRaw(Mem::Pid(), addr, zero, size);
}

void RestoreAll() {
    for (auto& kv : g_pinRestore) {
        Mem::WriteRaw(Mem::Pid(), kv.first, &kv.second, 4);
    }
    g_pinRestore.clear();
}

void PinRecoilOn(uintptr_t model) {
    // RecoilModel on client model: nominal offset in client tree
    if (!Mem::IsValidPtr(model)) return;
    uintptr_t rc = Mem::ReadPtr(model + 0x58); // nominal recoil slot
    if (Mem::IsValidPtr(rc)) PinZero(rc + 0x18, 12);
}

void PinSpreadOn(uintptr_t model) {
    if (!Mem::IsValidPtr(model)) return;
    uintptr_t sp = Mem::ReadPtr(model + 0x60);
    if (Mem::IsValidPtr(sp)) {
        PinZero(sp + 0x18, 4);
        PinZero(sp + 0x1C, 4);
        PinZero(sp + 0x20, 4);
        PinZero(sp + 0x24, 4);
    }
}

void PinSwayOn(uintptr_t model) {
    if (!Mem::IsValidPtr(model)) return;
    uintptr_t sw = Mem::ReadPtr(model + 0x68);
    if (Mem::IsValidPtr(sw)) {
        PinZero(sw + 0x18, 4);
        PinZero(sw + 0x1C, 4);
    }
}

// ==========================================================================
// Scanner loop
// ==========================================================================
void ScannerLoop() {
    while (g_scanRunning) {
        ResolveAllKlasses();

        if (!g_rootsValid) {
            if (TryGomPath() || TryStaticFieldPath() || TryHeapScan()) {
                if (g_battleEntities) {
                    g_worldEntities = Mem::ReadPtr(g_battleEntities + offsets::battle_entities_model::world_entities);
                }
                if (ValidateBattleModel(g_battleModel)) {
                    g_rootsValid = true;
                    Console::Success("roots: bm=%p be=%p we=%p",
                                     (void*)g_battleModel, (void*)g_battleEntities, (void*)g_worldEntities);
                }
            }
        } else {
            if (!ValidateBattleModel(g_battleModel)) {
                g_rootsValid = false;
                g_battleModel = g_battleEntities = g_worldEntities = 0;
                Console::Warn("roots invalidated");
            }
        }

        FindViewUpdater();

        std::this_thread::sleep_for(std::chrono::milliseconds(g_rootsValid ? 2000 : 500));
    }
}

} // namespace

// ==========================================================================
// Public API
// ==========================================================================
DWORD Game::FindProcess() {
    static const char* candidates[] = { "ROB RUN.exe", "RobRun.exe", "robrun.exe", "LokoLoko.exe" };
    for (auto* n : candidates) {
        DWORD pid = Game_FindProcessByName(n);
        if (pid) { std::strncpy(g_procName, n, 63); return pid; }
    }
    return 0;
}

bool Game::Init() {
    g_gameAsm   = Mem::GetModuleBase(L"GameAssembly.dll");
    g_unityBase = Mem::GetModuleBase(L"UnityPlayer.dll");
    if (!g_gameAsm) {
        Console::Error("Game::Init: GameAssembly.dll not mapped");
        return false;
    }
    RefreshWindowImpl();
    return true;
}

bool Game::IsAlive() {
    return Mem::IsConnected();
}

void Game::Update() {
    if (!Mem::IsConnected()) return;
    static int wndTick = 0;
    if ((wndTick++ % 60) == 0) RefreshWindowImpl();

    UpdateCameraImpl();

    auto now = Clock::now();
    if (now - g_lastCharGather > std::chrono::milliseconds(50)) {
        g_lastCharGather = now;
        std::vector<Player> p;
        GatherCharacters(p);
        std::lock_guard<std::mutex> g(g_playerMutex);
        g_players = std::move(p);
    }
    if (now - g_lastLootGather > std::chrono::milliseconds(600)) {
        g_lastLootGather = now;
        std::vector<LootItem>  l;
        std::vector<WorldMark> m;
        GatherLoot(l);
        GatherMarks(m);
        std::lock_guard<std::mutex> g(g_playerMutex);
        g_loot = std::move(l);
        g_marks = std::move(m);
    }

    ApplyTeleport(true);
}

void Game::StartScanner() {
    if (g_scanRunning) return;
    g_scanRunning = true;
    g_scanThread = std::thread(ScannerLoop);
}

void Game::StopScanner() {
    g_scanRunning = false;
    if (g_scanThread.joinable()) g_scanThread.join();
    RestoreAll();
}

HWND Game::GameWindow() { return g_hwnd; }
int  Game::ScreenW()    { return g_scrW; }
int  Game::ScreenH()    { return g_scrH; }
void Game::RefreshWindow() { RefreshWindowImpl(); }

Cam  Game::Camera() { return GetCameraForUse(); }

bool Game::ProjectPoint(const Vec3& w, float out[2], float* d, bool cull) { return ProjectImpl(w, out, d, cull); }
bool Game::WorldToScreen(const Vec3& w, float out[2])                     { return ProjectImpl(w, out, nullptr, true); }
bool Game::Project(const Vec3& w, float out[2], float* d)                 { return ProjectImpl(w, out, d, false); }

void Game::SnapshotPlayers(std::vector<Player>& out) {
    std::lock_guard<std::mutex> g(g_playerMutex);
    out = g_players;
}
void Game::SnapshotLoot(std::vector<LootItem>& out) {
    std::lock_guard<std::mutex> g(g_playerMutex);
    out = g_loot;
}
void Game::SnapshotMarks(std::vector<WorldMark>& out) {
    std::lock_guard<std::mutex> g(g_playerMutex);
    out = g_marks;
}
int Game::LocalPlayerIndex(const std::vector<Player>& v) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i].isLocalPlayer) return (int)i;
    return -1;
}

void Game::BeginTeleport(TpKind k, uint32_t entityId, const Vec3& point, bool fly) {
    g_tp.kind = k;
    g_tp.entityId = entityId;
    g_tp.point = point;
    g_tp.fly = fly;
    g_tp.flyVel = Vec3();
    g_tp.active = (k != TpKind::None);
}

void Game::CancelTeleport() { g_tp.active = false; g_tp.kind = TpKind::None; }

void Game::ApplyTeleport(bool steer) {
    if (!g_tp.active) return;
    uintptr_t local = LocalModel();
    if (!local) return;

    std::vector<Player> snap;
    SnapshotPlayers(snap);

    if (g_tp.kind == TpKind::ToEntity || g_tp.kind == TpKind::PullEntity) {
        for (auto& p : snap) {
            if (p.entityId != g_tp.entityId) continue;
            if (g_tp.kind == TpKind::ToEntity) {
                WriteModelPos(local, p.root);
            } else {
                Cam c = GetCameraForUse();
                WriteModelPos(p.model, c.valid ? c.pos : p.root);
            }
            return;
        }
    } else if (g_tp.kind == TpKind::ToPoint) {
        Vec3 pt = g_tp.point;
        if (steer) SteerFly(pt);
        g_tp.point = pt;
        WriteModelPos(local, pt);
    }
}

void Game::ReinforceTeleport() {
    if (!g_tp.active) return;
    ApplyTeleport(false);
}

void Game::ApplyGun(const GunToggles& t) {
    uintptr_t local = LocalModel();
    if (!local) {
        if (!t.noRecoil && !t.noSpread && !t.noSway && !g_pinRestore.empty()) RestoreAll();
        g_gunState = t;
        return;
    }
    if (t.noRecoil) PinRecoilOn(local); else if (g_gunState.noRecoil) RestoreAll();
    if (t.noSpread) PinSpreadOn(local);
    if (t.noSway)   PinSwayOn(local);
    g_gunState = t;
}

void Game::ObserveAimTurn(const Vec3& fwd) {
    float yaw   = std::atan2(fwd.x, fwd.z);
    float pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
    if (!g_sensHaveLast) {
        g_lastYaw = yaw;
        g_lastPitch = pitch;
        g_sensHaveLast = true;
        return;
    }
    float dyaw   = yaw - g_lastYaw;
    float dpitch = pitch - g_lastPitch;
    if (dyaw >  3.14159f) dyaw -= 6.2831853f;
    if (dyaw < -3.14159f) dyaw += 6.2831853f;
    if (std::fabs(g_lastAimDx) > 1.0f || std::fabs(g_lastAimDy) > 1.0f) {
        SensSample s{ (double)dyaw * (180.0 / 3.14159265), (double)dpitch * (180.0 / 3.14159265), g_lastAimDx, g_lastAimDy };
        g_sensSamples.push_back(s);
        if (g_sensSamples.size() > 32) g_sensSamples.erase(g_sensSamples.begin());
        if (g_sensSamples.size() >= 6) {
            double sxy = 0, sx2 = 0, syy = 0, sy2 = 0;
            for (auto& ss : g_sensSamples) {
                if (std::fabs(ss.dxRot) > 0.05) { sxy += ss.mx * ss.dxRot; sx2 += ss.dxRot * ss.dxRot; }
                if (std::fabs(ss.dyRot) > 0.05) { syy += ss.my * ss.dyRot; sy2 += ss.dyRot * ss.dyRot; }
            }
            if (sx2 > 0.001) g_cpdX = (float)(sxy / sx2);
            if (sy2 > 0.001) g_cpdY = (float)(syy / sy2);
        }
    }
    g_lastYaw = yaw;
    g_lastPitch = pitch;
    g_lastAimDx = g_lastAimDy = 0.0f;
}

void Game::ReportAimMove(float dx, float dy) {
    g_lastAimDx = dx;
    g_lastAimDy = dy;
}

void Game::ScaleAimMove(float& mx, float& my) {
    if (g_cpdX > 1.0f) mx *= (g_cpdX / 10.0f);
    if (std::fabs(g_cpdY) > 1.0f) my *= (g_cpdY / 10.0f);
}

float Game::Distance3D(const Vec3& a, const Vec3& b) {
    float dx = a.x-b.x, dy = a.y-b.y, dz = a.z-b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}
