#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Clip {

std::string Base64Encode(const uint8_t* data, size_t size);
bool        Base64Decode(const std::string& b64, std::vector<uint8_t>& out);

bool        SetText(const std::string& text);
std::string GetText();

} // namespace Clip
