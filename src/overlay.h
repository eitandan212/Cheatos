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

} // namespace Overlay
