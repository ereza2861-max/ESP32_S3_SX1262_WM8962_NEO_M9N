#pragma once

#include <array>
#include <cstdint>

namespace ProfileBinding {

using Tag = std::array<uint8_t, 32>;

Tag firmwareProfileHash();
bool isZero(const Tag& tag);
bool equal(const Tag& a, const Tag& b);

}  // namespace ProfileBinding
