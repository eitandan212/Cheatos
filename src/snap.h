#pragma once

#include <string>

namespace Snap {
// Save a PNG screenshot of the current overlay backbuffer to disk.
// Returns the written path or empty string on failure.
std::string Save();
}
