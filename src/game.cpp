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

// per-entity view-updater lookup (entity_id -> native view pointer)
std::unordered_map<uint32_t, uintptr_t> g_viewForEntity;

// memory-aim state
struct AimState {
    bool  active = false;
    Vec3  target;
    double untilSec = 0.0;
};
AimState g_aim;

// recoil/spread/sway pins. A pin records the exact bytes it overwrote so the
// restore is byte-exact, and the owning model so a pin is never written back
// to an address whose object has already been destroyed.
constexpr size_t kPinMax = 32;
struct Pin {
    uintptr_t addr  = 0;
    uint32_t  size  = 0;
    uint8_t   owner = 0;        // bit 0 recoil, 1 spread, 2 sway
    uint8_t   orig[kPinMax]{};
};
std::vector<Pin>  g_pins;
uintptr_t         g_pinModel = 0;      // model the pins belong to
Game::GunToggles  g_gunState;

// sensitivity calibration
struct SensSample { double dxRot, dyRot, mx, my; };
std::vector<SensSample> g_sensSamples;
float g_cpdX = 0.0f, g_cpdY = 0.0f;
float g_lastYaw = 0.0f, g_lastPitch = 0.0f;
bool  g_sensHaveLast = false;
float g_lastAimDx = 0.0f, g_lastAimDy = 0.0f;

// klass resolution bookkeeping: a name that never matches is given up on so
// the scanner stops re-sweeping the address space for it every cycle.
constexpr int kKlassMaxTries = 3;
std::unordered_map<std::string, int> g_klassTries;
std::unordered_set<std::string>      g_klassGated;
int  g_klassArenaMisses = 0;

// teleport rejection tracking (server pulled us back)
constexpr int   kTpRejectLimit    = 10;     // consecutive reverted frames
constexpr float kTpRejectDistance = 2.5f;   // metres of drift that counts as a revert
int  g_tpRejectRun   = 0;
int  g_tpRejectTotal = 0;
Vec3 g_tpLastWrite;
bool g_tpHaveLastWrite = false;

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
    auto t0 = Clock::now();
    const auto budget = std::chrono::milliseconds(2500);
    MEMORY_BASIC_INFORMATION mbi;
    // Start at the arena guess, then fall through to the whole private range.
    // Once the arena has come up empty twice we stop paying for that pass.
    uintptr_t addr = offsets::klass_arena_lo;
    uintptr_t hi   = offsets::klass_arena_hi;
    int phase = 0;
    if (g_klassArenaMisses >= 2) { addr = 0x10000; hi = 0x7FFFFFFFFFFFULL; phase = 1; }
    while (out.size() < wanted.size()) {
        if (Clock::now() - t0 > budget) break;
        if (addr >= hi) {
            if (phase++ == 0) { addr = 0x10000; hi = 0x7FFFFFFFFFFFULL; }
            else break;
        }
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
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    CloseHandle(h);
    if (phase == 0 && out.empty() && g_klassArenaMisses < 2) {
        g_klassArenaMisses++;
        if (g_klassArenaMisses == 2)
            Console::Warn("klass arena window empty twice, full sweep only from now on");
    }
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
        if (dst) return;
        if (g_klassGated.find(n) != g_klassGated.end()) return;   // given up on
        wanted.push_back(n);
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

    // Anything still missing after this sweep burned a try. Three empty sweeps
    // and the name is gated: the feature behind it stays off and the scanner
    // stops paying 2.5 s per cycle looking for a name this build doesn't have.
    for (const auto& w : wanted) {
        if (hits.find(w) != hits.end()) continue;
        int t = ++g_klassTries[w];
        if (t >= kKlassMaxTries && g_klassGated.insert(w).second)
            Console::Warn("klass '%s' not present after %d sweeps, feature gated off", w.c_str(), t);
    }
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
    auto t0 = Clock::now();
    const auto budget = std::chrono::milliseconds(1500);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    bool found = false;
    size_t bytesSwept = 0;
    while (!found && addr < 0x7FFFFFFFFFFFULL) {
        if (Clock::now() - t0 > budget) break;
        if (bytesSwept > (size_t)1 << 30) break;              // 1 GiB hard cap per pass
        if (!VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && (mbi.Type & MEM_PRIVATE) &&
            !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD)) {
            size_t sz = (size_t)mbi.RegionSize;
            if (sz > 0x100000) sz = 0x100000;                 // 1 MiB per region
            bytesSwept += sz;
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
    auto t0 = Clock::now();
    const auto budget = std::chrono::milliseconds(1500);
    size_t bytesSwept = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    while ((!g_viewUpdater || !g_cineBrain) && addr < 0x7FFFFFFFFFFFULL) {
        if (Clock::now() - t0 > budget) break;
        if (bytesSwept > (size_t)1 << 30) break;
        if (!VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && (mbi.Type & MEM_PRIVATE) &&
            !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD)) {
            size_t sz = (size_t)mbi.RegionSize;
            if (sz > 0x100000) sz = 0x100000;
            bytesSwept += sz;
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
    // Past the hold window the last good basis is no longer evidence about
    // where the camera is pointing. Report invalid rather than project from it.
    return Cam{};
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
    if (!Mem::IsValidPtr(entries) || count <= 0) return;
    if (count > (int32_t)cap) count = (int32_t)cap;   // read the head, don't drop the dict
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

bool ReadViewHead(uintptr_t view, Vec3& out) {
    if (!Mem::IsValidPtr(view)) return false;
    // path 1: head_position transform
    uintptr_t headT = Mem::ReadPtr(view + offsets::char_view::head_position);
    if (Mem::IsValidPtr(headT)) {
        Vec3 h = WorldFromTransform(headT);
        if (InWorld(h)) { out = h; return true; }
    }
    // path 2: head_behavior -> bone_target transform
    uintptr_t behavior = Mem::ReadPtr(view + offsets::char_view::head_behavior);
    if (Mem::IsValidPtr(behavior)) {
        uintptr_t bone = Mem::ReadPtr(behavior + offsets::char_view::bone_target);
        if (Mem::IsValidPtr(bone)) {
            Vec3 h = WorldFromTransform(bone);
            if (InWorld(h)) { out = h; return true; }
        }
    }
    return false;
}

void FillRig(uintptr_t view, BoneRig& rig, Vec3 root) {
    rig.filled = false;
    for (auto& j : rig.joints) j = root;
    Vec3 liveHead;
    bool haveHead = ReadViewHead(view, liveHead);
    if (haveHead) rig.joints[3] = liveHead;
    else          rig.joints[3] = Vec3(root.x, root.y + offsets::skeleton::head_height, root.z);
    // anchor the rest to root + measured head so proportions follow the real height
    float torsoY = root.y + (haveHead ? (rig.joints[3].y - root.y) * 0.82f : 1.35f);
    float pelvisY= root.y + (haveHead ? (rig.joints[3].y - root.y) * 0.55f : 0.90f);
    rig.joints[1]  = Vec3(root.x, torsoY, root.z);
    rig.joints[2]  = Vec3(root.x, rig.joints[3].y - 0.10f, root.z);
    float shoulderY = torsoY;
    float elbowY    = torsoY - 0.25f;
    float handY     = torsoY - 0.45f;
    rig.joints[4]  = Vec3(root.x - 0.20f, shoulderY, root.z);
    rig.joints[5]  = Vec3(root.x + 0.20f, shoulderY, root.z);
    rig.joints[6]  = Vec3(root.x - 0.30f, elbowY,    root.z);
    rig.joints[7]  = Vec3(root.x + 0.30f, elbowY,    root.z);
    rig.joints[8]  = Vec3(root.x - 0.32f, handY,     root.z);
    rig.joints[9]  = Vec3(root.x + 0.32f, handY,     root.z);
    rig.joints[10] = Vec3(root.x - 0.12f, pelvisY - 0.05f, root.z);
    rig.joints[11] = Vec3(root.x + 0.12f, pelvisY - 0.05f, root.z);
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

    uintptr_t view = 0;
    if (auto it = g_viewForEntity.find((uint32_t)entityId); it != g_viewForEntity.end())
        view = it->second;
    FillRig(view, p.rig, root);
    if (view) {
        Vec3 h;
        if (ReadViewHead(view, h)) p.head = h;
    }
    p.hasRig = p.rig.filled;

    Cam c = GetCameraForUse();
    if (c.valid) p.distance = Game::Distance3D(c.pos, root);

    p.lastSeen = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    out.push_back(std::move(p));
}

void RebuildViewMap() {
    g_viewForEntity.clear();
    if (!g_viewUpdater) return;
    uintptr_t dict = Mem::ReadPtr(g_viewUpdater + offsets::entities_updater::view_dict);
    std::vector<std::pair<int32_t, uintptr_t>> entries;
    IterateDict(dict, entries);
    for (auto& e : entries) g_viewForEntity[(uint32_t)e.first] = e.second;
}

// A full gather walks dictionaries and reads names, roles and held items, which
// is why it runs at 20 Hz. Positions, heads, rigs and health change every frame
// though, and boxes drawn from 50 ms-old positions visibly lag the player. This
// re-reads just the per-frame fields of entities already known.
void RefreshSnapshot(std::vector<Player>& players) {
    Cam c = GetCameraForUse();
    const double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    for (auto& p : players) {
        if (!p.model) continue;
        Vec3 root;
        if (!ReadPosition(p.model, root)) continue;      // partial read: keep last good
        p.root = root;
        g_posMem[(uint64_t)p.model] = root;

        Vec3 head, foot;
        ReadHitboxBones(p.model, root, head, foot);
        p.head = head;

        uintptr_t view = 0;
        if (auto it = g_viewForEntity.find(p.entityId); it != g_viewForEntity.end())
            view = it->second;
        if (view) {
            FillRig(view, p.rig, root);
            p.hasRig = p.rig.filled;
            Vec3 h;
            if (ReadViewHead(view, h)) p.head = h;
        }

        ReadHealth(p.model, p.isLocalPlayer, p.health, p.maxHealth);
        p.alive = p.health > 0;
        if (c.valid) p.distance = Game::Distance3D(c.pos, root);
        p.lastSeen = now;
    }
}

void GatherCharacters(std::vector<Player>& out) {
    out.clear();
    if (!g_battleEntities) return;

    RebuildViewMap();

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
// Pin helpers
// ==========================================================================
// A pin zeroes a field every frame and remembers the exact bytes it replaced.
// Two rules make it safe: nothing is written until the bytes read back as
// plausible floats, and pins are dropped (never written back) once the model
// that owned them is gone, because that address may already be reused.
bool PinLooksLikeFloats(uintptr_t addr, size_t size) {
    if (size < 4 || size > kPinMax || (size % 4) != 0) return false;
    float f[kPinMax / 4]{};
    if (!Mem::ReadRaw(Mem::Pid(), addr, f, size)) return false;
    const size_t n = size / 4;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(f[i])) return false;
        if (std::fabs(f[i]) > 1.0e5f) return false;
    }
    return true;
}

int PinFind(uintptr_t addr) {
    for (size_t i = 0; i < g_pins.size(); ++i)
        if (g_pins[i].addr == addr) return (int)i;
    return -1;
}

void PinApply(uintptr_t addr, size_t size, uint8_t ownerBit) {
    if (!Mem::IsValidPtr(addr) || size == 0 || size > kPinMax) return;
    int idx = PinFind(addr);
    if (idx < 0) {
        if (!PinLooksLikeFloats(addr, size)) return;   // offset is wrong, do not scribble
        if (g_pins.size() >= 64) return;
        Pin pin;
        pin.addr = addr;
        pin.size = (uint32_t)size;
        pin.owner = ownerBit;
        if (!Mem::ReadRaw(Mem::Pid(), addr, pin.orig, size)) return;
        g_pins.push_back(pin);
        idx = (int)g_pins.size() - 1;
    } else {
        g_pins[(size_t)idx].owner |= ownerBit;
    }
    uint8_t zero[kPinMax] = {};
    Mem::WriteRaw(Mem::Pid(), addr, zero, size);
}

void PinReleaseOwner(uint8_t ownerBit, bool writeBack) {
    for (size_t i = 0; i < g_pins.size();) {
        Pin& pin = g_pins[i];
        if (!(pin.owner & ownerBit)) { ++i; continue; }
        pin.owner = (uint8_t)(pin.owner & ~ownerBit);
        if (pin.owner == 0) {
            if (writeBack) Mem::WriteRaw(Mem::Pid(), pin.addr, pin.orig, pin.size);
            g_pins.erase(g_pins.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void PinRecoilOn(uintptr_t model) {
    // [unverified | model + 0x58 -> recoil model, value at +0x18 as Vec3]
    // Guessed layout: PinApply refuses the write unless the three floats read
    // back plausible, so a wrong offset costs a skipped feature, not a crash.
    if (!Mem::IsValidPtr(model)) return;
    uintptr_t rc = Mem::ReadPtr(model + 0x58);
    if (Mem::IsValidPtr(rc)) PinApply(rc + 0x18, 12, 0x01);
}

void PinSpreadOn(uintptr_t model) {
    // [unverified | model + 0x60 -> spread model, four floats from +0x18]
    if (!Mem::IsValidPtr(model)) return;
    uintptr_t sp = Mem::ReadPtr(model + 0x60);
    if (Mem::IsValidPtr(sp)) PinApply(sp + 0x18, 16, 0x02);
}

void PinSwayOn(uintptr_t model) {
    // [unverified | model + 0x68 -> sway model, two floats from +0x18]
    if (!Mem::IsValidPtr(model)) return;
    uintptr_t sw = Mem::ReadPtr(model + 0x68);
    if (Mem::IsValidPtr(sw)) PinApply(sw + 0x18, 8, 0x04);
}

// Prune position memory so a long session doesn't grow it without bound.
void PrunePosMem() {
    if (g_posMem.size() <= 512) return;
    g_posMem.clear();
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
    } else {
        std::lock_guard<std::mutex> g(g_playerMutex);
        RefreshSnapshot(g_players);
    }
    PrunePosMem();
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
    ReleaseGun();
}

HWND Game::GameWindow() { return g_hwnd; }
int  Game::ScreenW()    { return g_scrW; }
int  Game::ScreenH()    { return g_scrH; }
void Game::RefreshWindow() { RefreshWindowImpl(); }

void Game::ScreenCenter(float& cx, float& cy) {
    // Client-space centre. Projection works in client space, so every aim and
    // crosshair calculation has to read its centre from the same rectangle.
    cx = (float)g_scrW * 0.5f;
    cy = (float)g_scrH * 0.5f;
}

bool Game::IsForeground() {
    if (!g_hwnd) return false;
    HWND fg = GetForegroundWindow();
    return fg == g_hwnd;
}

Game::Health Game::Diagnostics() {
    Health h;
    h.rootsValid  = g_rootsValid.load();
    h.cameraValid = GetCameraForUse().valid;
    const uintptr_t* k = (const uintptr_t*)&g_k;
    h.klassTotal = (int)(sizeof(Klasses) / sizeof(uintptr_t));
    for (int i = 0; i < h.klassTotal; ++i) if (k[i]) h.klassFound++;
    h.klassGated = (int)g_klassGated.size();
    {
        std::lock_guard<std::mutex> g(g_playerMutex);
        h.players = (int)g_players.size();
    }
    h.tpRejects = g_tpRejectTotal;
    return h;
}

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
    g_tpRejectRun = 0;
    g_tpHaveLastWrite = false;
}

void Game::CancelTeleport() {
    g_tp.active = false;
    g_tp.kind = TpKind::None;
    g_tpRejectRun = 0;
    g_tpHaveLastWrite = false;
}

bool Game::TeleportRejected()    { return g_tpRejectRun >= kTpRejectLimit; }
int  Game::TeleportRejectCount() { return g_tpRejectTotal; }

void Game::ApplyTeleport(bool steer) {
    if (!g_tp.active) return;
    uintptr_t local = LocalModel();
    if (!local) return;

    // A write that the server does not accept is pulled back within a tick or
    // two. Compare where we asked to be against where we actually are, and if
    // the position keeps reverting, say so once and stop fighting it.
    if (g_tpHaveLastWrite && g_tp.kind != TpKind::PullEntity) {
        Vec3 now;
        if (ReadPosition(local, now)) {
            const float drift = Game::Distance3D(now, g_tpLastWrite);
            if (drift > kTpRejectDistance) {
                if (++g_tpRejectRun == kTpRejectLimit) {
                    g_tpRejectTotal++;
                    Console::Warn("teleport reverted %d frames running (%.1fm), server is authoritative here",
                                  g_tpRejectRun, drift);
                    g_tp.active = false;
                    g_tp.kind = TpKind::None;
                    g_tpHaveLastWrite = false;
                    return;
                }
            } else if (g_tpRejectRun) {
                g_tpRejectRun = 0;
            }
        }
    }

    std::vector<Player> snap;
    SnapshotPlayers(snap);

    if (g_tp.kind == TpKind::ToEntity || g_tp.kind == TpKind::PullEntity) {
        for (auto& p : snap) {
            if (p.entityId != g_tp.entityId) continue;
            if (g_tp.kind == TpKind::ToEntity) {
                WriteModelPos(local, p.root);
                g_tpLastWrite = p.root;
                g_tpHaveLastWrite = true;
            } else {
                Cam c = GetCameraForUse();
                WriteModelPos(p.model, c.valid ? c.pos : p.root);
            }
            return;
        }
    } else if (g_tp.kind == TpKind::ToPoint) {
        Vec3 pt = g_tp.point;
        if (steer) SteerFly(pt);
        if (!InWorld(pt)) { g_tp.active = false; return; }
        g_tp.point = pt;
        WriteModelPos(local, pt);
        g_tpLastWrite = pt;
        g_tpHaveLastWrite = true;
    }
}

void Game::ReinforceTeleport() {
    if (!g_tp.active) return;
    ApplyTeleport(false);
}

void Game::ApplyGun(const GunToggles& t) {
    // Records intent only. TickGun does the work every frame so a pin survives
    // a respawn, a weapon swap and the game writing the field back.
    g_gunState = t;
    TickGun();
}

void Game::TickGun() {
    const GunToggles t = g_gunState;
    uintptr_t local = LocalModel();

    if (!local) {
        // The model is gone. Its pin addresses may already be reused, so the
        // records are dropped without writing anything back.
        if (!g_pins.empty()) g_pins.clear();
        g_pinModel = 0;
        return;
    }
    if (local != g_pinModel) {
        if (g_pinModel) g_pins.clear();   // same reason: old object is dead
        g_pinModel = local;
    }

    // Features switched off give their bytes back while the model is still live.
    if (!t.noRecoil) PinReleaseOwner(0x01, true);
    if (!t.noSpread) PinReleaseOwner(0x02, true);
    if (!t.noSway)   PinReleaseOwner(0x04, true);

    if (t.noRecoil) PinRecoilOn(local);
    if (t.noSpread) PinSpreadOn(local);
    if (t.noSway)   PinSwayOn(local);
}

void Game::ReleaseGun() {
    if (g_pins.empty()) return;
    const bool alive = Mem::IsConnected() && g_pinModel && LocalModel() == g_pinModel;
    PinReleaseOwner(0x01, alive);
    PinReleaseOwner(0x02, alive);
    PinReleaseOwner(0x04, alive);
    g_pins.clear();
    g_pinModel = 0;
}

// --------------------------------------------------------------------------
// Sensitivity
//
// The old path multiplied a pixel delta by countsPerDegree/10, which mixes two
// different units and leaves a magic constant in the aim loop. The aim path now
// works in degrees end to end: the error to the target is an angle, the mouse
// takes counts, and counts-per-degree is the single conversion between them.
//
// Counts-per-degree is measured from real moves by least squares. Until enough
// samples land it is estimated from vertical FOV and client height, which is a
// linearisation about screen centre, not a measurement.
// --------------------------------------------------------------------------
namespace {

constexpr float kDeg = 57.295779513f;      // 180/pi
constexpr int   kSensMinSamples = 8;
constexpr size_t kSensMaxSamples = 48;

float EstimateCountsPerDeg() {
    Cam c = GetCameraForUse();
    const float fov = (c.valid && c.fov > 20.0f && c.fov < 170.0f) ? c.fov : offsets::camera_fov_degrees;
    const int   h   = g_scrH > 0 ? g_scrH : 1080;
    const float tanHalf = std::tan(fov * 0.5f / kDeg);
    if (tanHalf < 1e-4f) return 10.0f;
    return ((float)h * 0.5f / tanHalf) / kDeg;   // pixels per degree at centre
}

float ClampCpd(float v) {
    const float a = std::fabs(v);
    if (!std::isfinite(v) || a < 0.5f || a > 2000.0f) return 0.0f;
    return v;
}

} // namespace

void Game::ObserveAimTurn(const Vec3& fwd) {
    float yaw   = std::atan2(fwd.x, fwd.z) * kDeg;
    float pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f)) * kDeg;
    if (!g_sensHaveLast) {
        g_lastYaw = yaw;
        g_lastPitch = pitch;
        g_sensHaveLast = true;
        g_lastAimDx = g_lastAimDy = 0.0f;
        return;
    }
    float dyaw   = yaw - g_lastYaw;
    float dpitch = pitch - g_lastPitch;
    if (dyaw >  180.0f) dyaw -= 360.0f;
    if (dyaw < -180.0f) dyaw += 360.0f;

    const bool moved = std::fabs(g_lastAimDx) > 1.0f || std::fabs(g_lastAimDy) > 1.0f;
    // A respawn, a cut or a teleport swings the camera far further than one
    // mouse step could have. Those frames are not evidence about sensitivity.
    const bool sane = std::fabs(dyaw) < 25.0f && std::fabs(dpitch) < 25.0f;

    if (moved && sane) {
        SensSample smp{ (double)dyaw, (double)dpitch, g_lastAimDx, g_lastAimDy };
        bool keep = true;
        if (g_cpdX > 0.0f && std::fabs(dyaw) > 0.05) {
            const double implied = std::fabs(smp.mx / dyaw);
            const double ratio   = implied / (double)g_cpdX;
            if (ratio < 0.25 || ratio > 4.0) keep = false;    // outlier
        }
        if (keep) {
            g_sensSamples.push_back(smp);
            if (g_sensSamples.size() > kSensMaxSamples)
                g_sensSamples.erase(g_sensSamples.begin());
        }
        if (g_sensSamples.size() >= (size_t)kSensMinSamples) {
            double sxy = 0, sx2 = 0, syy = 0, sy2 = 0;
            for (auto& ss : g_sensSamples) {
                if (std::fabs(ss.dxRot) > 0.05) { sxy += ss.mx * ss.dxRot; sx2 += ss.dxRot * ss.dxRot; }
                if (std::fabs(ss.dyRot) > 0.05) { syy += ss.my * ss.dyRot; sy2 += ss.dyRot * ss.dyRot; }
            }
            if (sx2 > 0.001) g_cpdX = ClampCpd((float)(sxy / sx2));
            if (sy2 > 0.001) g_cpdY = ClampCpd((float)(syy / sy2));
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

bool Game::SensCalibrated() {
    return std::fabs(g_cpdX) > 0.5f && std::fabs(g_cpdY) > 0.5f &&
           g_sensSamples.size() >= (size_t)kSensMinSamples;
}

void Game::SensCountsPerDeg(float& x, float& y) {
    if (SensCalibrated()) { x = g_cpdX; y = g_cpdY; return; }
    const float est = EstimateCountsPerDeg();
    x =  est;
    y = -est;      // screen y grows downward, pitch grows upward
}

int Game::SensSampleCount() { return (int)g_sensSamples.size(); }

void Game::ResetSensCalibration() {
    g_sensSamples.clear();
    g_cpdX = g_cpdY = 0.0f;
    g_sensHaveLast = false;
}

bool Game::AimDeltaDegrees(const Vec3& target, float& dYawDeg, float& dPitchDeg) {
    dYawDeg = dPitchDeg = 0.0f;
    Cam c = GetCameraForUse();
    if (!c.valid) return false;
    Vec3 d = Sub(target, c.pos);
    if (!IsFiniteVec(d)) return false;
    const float len = Length(d);
    if (len < 1e-3f) return false;
    d = Mul(d, 1.0f / len);
    // Angles are taken against the camera basis, so this holds for a target
    // behind the player or off the edge of the screen, where projection fails.
    const float yawT   = std::atan2(d.x, d.z) * kDeg;
    const float yawC   = std::atan2(c.fwd.x, c.fwd.z) * kDeg;
    const float pitchT = std::asin(std::clamp(d.y,     -1.0f, 1.0f)) * kDeg;
    const float pitchC = std::asin(std::clamp(c.fwd.y, -1.0f, 1.0f)) * kDeg;
    float dy = yawT - yawC;
    if (dy >  180.0f) dy -= 360.0f;
    if (dy < -180.0f) dy += 360.0f;
    dYawDeg   = dy;
    dPitchDeg = pitchT - pitchC;
    return true;
}

void Game::AnglesToCounts(float dYawDeg, float dPitchDeg, float& mx, float& my) {
    float cx = 0, cy = 0;
    SensCountsPerDeg(cx, cy);
    mx = dYawDeg   * cx;
    my = dPitchDeg * cy;
}

void Game::ScaleAimMove(float& mx, float& my) {
    // Legacy pixel path. Pixels are already the right unit for a 1:1 mouse, and
    // the calibrated conversion now lives in AnglesToCounts, so this no longer
    // folds counts-per-degree into a pixel delta.
    (void)mx; (void)my;
}

float Game::Distance3D(const Vec3& a, const Vec3& b) {
    float dx = a.x-b.x, dy = a.y-b.y, dz = a.z-b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

static bool WriteCameraBasisTo(const Vec3& target) {
    if (!g_battleModel) return false;
    uintptr_t cam = Mem::ReadPtr(g_battleModel + offsets::battle_model::camera);
    if (!Mem::IsValidPtr(cam)) return false;
    Vec3 pos = ReadVec3(cam + offsets::battle_camera_model::position);
    if (!InWorld(pos)) return false;
    Vec3 fwd = Normalize(Sub(target, pos));
    if (!Unitish(fwd)) return false;
    // Keep the basis out of the gimbal: a near-vertical forward makes right and
    // up degenerate and the view snaps.
    if (std::fabs(fwd.y) > 0.9995f) return false;
    Vec3 worldUp(0, 1, 0);
    Vec3 right;
    // right = normalize(cross(worldUp, fwd))
    right.x = worldUp.y * fwd.z - worldUp.z * fwd.y;
    right.y = worldUp.z * fwd.x - worldUp.x * fwd.z;
    right.z = worldUp.x * fwd.y - worldUp.y * fwd.x;
    if (Length(right) < 1e-4f) right = Vec3(1, 0, 0);
    right = Normalize(right);
    Vec3 up;
    up.x = fwd.y * right.z - fwd.z * right.y;
    up.y = fwd.z * right.x - fwd.x * right.z;
    up.z = fwd.x * right.y - fwd.y * right.x;
    up = Normalize(up);
    bool ok = true;
    ok &= Mem::WriteRaw(Mem::Pid(), cam + offsets::battle_camera_model::forward, &fwd,   sizeof(fwd));
    ok &= Mem::WriteRaw(Mem::Pid(), cam + offsets::battle_camera_model::right,   &right, sizeof(right));
    ok &= Mem::WriteRaw(Mem::Pid(), cam + offsets::battle_camera_model::up,      &up,    sizeof(up));
    return ok;
}

bool Game::MemoryAim(const Vec3& worldTarget) {
    g_aim.active   = true;
    g_aim.target   = worldTarget;
    g_aim.untilSec = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() + 0.08;
    return WriteCameraBasisTo(worldTarget);
}

void Game::ReinforceAim() {
    if (!g_aim.active) return;
    double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    if (now > g_aim.untilSec) { g_aim.active = false; return; }
    WriteCameraBasisTo(g_aim.target);
}

bool Game::WriteCameraFov(float deg) {
    if (!g_cineBrain || deg < 20.0f || deg > 170.0f) return false;
    return Mem::WriteRaw(Mem::Pid(), g_cineBrain + offsets::cinemachine_brain::fov, &deg, sizeof(deg));
}

bool Game::WriteThirdPerson(float back, float upOff) {
    if (!g_battleModel) return false;
    uintptr_t cam = Mem::ReadPtr(g_battleModel + offsets::battle_model::camera);
    if (!Mem::IsValidPtr(cam)) return false;
    Vec3 p    = ReadVec3(cam + offsets::battle_camera_model::position);
    Vec3 fwd  = ReadVec3(cam + offsets::battle_camera_model::forward);
    Vec3 up   = ReadVec3(cam + offsets::battle_camera_model::up);
    if (!InWorld(p) || !Unitish(fwd) || !Unitish(up)) return false;
    Vec3 pulled(p.x - fwd.x * back + up.x * upOff,
                p.y - fwd.y * back + up.y * upOff,
                p.z - fwd.z * back + up.z * upOff);
    return Mem::WriteRaw(Mem::Pid(), cam + offsets::battle_camera_model::position, &pulled, sizeof(pulled));
}

void Game::DisableThirdPerson() {
    // no-op: without saving the original offset we can't restore without
    // the next game-camera-tick which happens automatically.
}

bool Game::WriteCrouch(bool crouched) {
    uintptr_t m = LocalModel();
    if (!m) return false;
    uintptr_t cr = Mem::ReadPtr(m + offsets::battle_character_model::crouch);
    if (!Mem::IsValidPtr(cr)) return false;
    uintptr_t pr = Mem::ReadPtr(cr + 0x10);
    if (!Mem::IsValidPtr(pr)) return false;
    uint8_t v = crouched ? 1 : 0;
    return Mem::WriteRaw(Mem::Pid(), pr + offsets::primitive::value, &v, 1);
}

bool Game::LocalPlayerRoot(Vec3& out) {
    uintptr_t m = LocalModel();
    if (!m) return false;
    return ReadPosition(m, out);
}
