#pragma once

namespace Console {
bool Init();
void Shutdown();
void Log(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);
void Success(const char* fmt, ...);
}
