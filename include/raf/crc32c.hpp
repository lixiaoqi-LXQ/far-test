#pragma once
#include "raf/raf.hpp"

namespace raf {
// Seed is a previously finalized CRC, permitting bounded-memory streaming.
std::uint32_t crc32c(ByteSpan bytes, std::uint32_t seed = 0) noexcept;
std::uint32_t crc32cPortable(ByteSpan bytes, std::uint32_t seed = 0) noexcept;
const char *crc32cBackend() noexcept;
} // namespace raf
