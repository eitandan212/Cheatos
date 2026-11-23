#pragma once

#include <windows.h>

namespace Overlay {

bool Init();
void Shutdown();

bool BeginFrame();
void EndFrame();

HWND Hwnd();
int  Width();
int  Height();

void MatchWindow(HWND game);
void SetClickThrough(bool through);

void ApplyThemeDarkRed();
void ApplyThemePreset(int preset);   // 0=Cyan 1=Rose 2=Amber 3=Purple 4=Mono
void SetMenuAlpha(float a);

} // namespace Overlay
