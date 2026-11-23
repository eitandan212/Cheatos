#pragma once

#include <cstdint>
#include <string>

namespace Portrait {

bool Init();
void Shutdown();
void RegisterAll();
void Register(const std::string& key, const uint8_t* bytes, size_t size);
void Draw(const std::string& key, float x, float y, float w, float h);

} // namespace Portrait
