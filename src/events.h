#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Events {

enum class Kind : uint8_t { Info, Aim, Trigger, Teleport, Config, Warn };

struct Entry {
    Kind        kind;
    std::string text;
    double      t;        // seconds since boot
};

void Push(Kind k, const char* fmt, ...);
const std::vector<Entry>& Snapshot();
void Clear();

} // namespace Events
