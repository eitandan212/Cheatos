#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellscalingapi.h>

#include "console.h"
#include "mem.h"
#include "game.h"
#include "overlay.h"
#include "esp.h"
#include "aimbot.h"
#include "menu.h"
#include "misc.h"
#include "portrait.h"
#include "state.h"
#include "hud.h"

#pragma comment(lib, "user32.lib")

static void EnableDpi() {
    HMODULE user = LoadLibraryW(L"user32.dll");
    if (!user) return;
    typedef BOOL(WINAPI* P)(DPI_AWARENESS_CONTEXT);
    auto fn = (P)GetProcAddress(user, "SetProcessDpiAwarenessContext");
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    else    SetProcessDPIAware();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    EnableDpi();

    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\YSWVExternal.Instance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"already running", L"loko loko", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    Console::Init();
    Console::Log("boot");

    // locate game
    DWORD pid = 0;
    for (int tries = 0; tries < 20 && !pid; ++tries) {
        pid = Game::FindProcess();
        if (!pid) Sleep(2000);
    }
    if (!pid) {
        Console::Error("game process not found");
        return 1;
    }

    if (!Mem::Init(pid)) {
        Console::Error("Mem::Init failed, check debug.log");
        return 2;
    }
    if (!Game::Init()) {
        Console::Error("Game::Init failed");
        return 3;
    }
    if (!Overlay::Init()) {
        Console::Error("Overlay::Init failed");
        return 4;
    }
    Overlay::ApplyThemePreset((int)AppState::Cfg().theme);
    Overlay::SetMenuAlpha(AppState::Cfg().menuAlpha);
    Portrait::Init();
    Portrait::RegisterAll();
    Menu::Init();
    Menu::LoadConfig();

    Game::StartScanner();
    Console::Success("ready");

    int tick = 0;
    bool prevMenuKey = false;
    bool prevExitKey = false;
    while (true) {
        bool insert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        bool endK   = (GetAsyncKeyState(VK_END)    & 0x8000) != 0;
        if (insert && !prevMenuKey) Menu::Toggle();
        if (endK   && !prevExitKey) break;
        prevMenuKey = insert;
        prevExitKey = endK;

        if ((tick++ % 15) == 0 && !Mem::IsConnected()) {
            Console::Warn("process gone");
            break;
        }

        Overlay::MatchWindow(Game::GameWindow());
        Overlay::SetClickThrough(!Menu::IsOpen());

        AppState::PollPanic();
        Game::Update();
        if (!AppState::Cfg().panicActive) {
            Misc::Update();
        } else {
            // Panic must leave the process as it found it: give back every
            // pinned byte and let go of any key being held.
            Game::ReleaseGun();
            Misc::ReleaseAll();
        }

        if (!Overlay::BeginFrame()) break;  // WM_QUIT only
        ESP::Render();
        if (!Menu::IsOpen() && !AppState::Cfg().panicActive) {
            Aimbot::Update();
            Aimbot::ApplyRcs();
        }
        Hud::Render();
        Menu::Render();
        if (!AppState::Cfg().panicActive) {
            Game::ReinforceTeleport();
            Game::ReinforceAim();
        }
        Hud::TickFps();
        Overlay::EndFrame();
    }

    Misc::ReleaseAll();
    Portrait::Shutdown();
    Overlay::Shutdown();
    Game::StopScanner();      // restores gun pins while the process is still open
    Mem::Shutdown();
    Console::Log("exit clean");
    Console::Shutdown();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
