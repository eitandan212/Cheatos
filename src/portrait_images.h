#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace PortraitImages {

struct Record {
    std::string    key;
    const uint8_t* data;
    size_t         size;
};

const std::vector<Record>& All();

} // namespace PortraitImages
