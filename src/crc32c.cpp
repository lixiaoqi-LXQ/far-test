#include "raf/crc32c.hpp"
#include <array>
#include <bit>
#include <cstring>
#if defined(__aarch64__) && !defined(RAF_PORTABLE_CRC)
#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(__x86_64__) && !defined(RAF_PORTABLE_CRC)
#include <nmmintrin.h>
#endif
namespace raf {
namespace {
constexpr auto makeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    auto crc = i;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
    table[i] = crc;
  }
  return table;
}
constexpr auto table = makeTable();
} // namespace
std::uint32_t crc32cPortable(ByteSpan bytes, std::uint32_t seed) noexcept {
  auto crc = ~seed;
  for (auto byte : bytes)
    crc =
        table[(crc ^ std::to_integer<std::uint32_t>(byte)) & 255] ^ (crc >> 8);
  return ~crc;
}
namespace {
#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(RAF_PORTABLE_CRC)
template <class T> T load(const std::byte *bytes) noexcept {
  T value;
  std::memcpy(&value, bytes, sizeof(value));
  if constexpr (std::endian::native == std::endian::big) {
    if constexpr (sizeof(T) == 8)
      value = __builtin_bswap64(value);
    if constexpr (sizeof(T) == 4)
      value = __builtin_bswap32(value);
    if constexpr (sizeof(T) == 2)
      value = __builtin_bswap16(value);
  }
  return value;
}
#if defined(__aarch64__)
__attribute__((target("+crc"))) std::uint32_t
hardware(ByteSpan bytes, std::uint32_t seed) noexcept {
  auto crc = ~seed;
  auto *data = bytes.data();
  auto size = bytes.size();
  while (size >= 32) {
    crc = __crc32cd(crc, load<std::uint64_t>(data));
    crc = __crc32cd(crc, load<std::uint64_t>(data + 8));
    crc = __crc32cd(crc, load<std::uint64_t>(data + 16));
    crc = __crc32cd(crc, load<std::uint64_t>(data + 24));
    data += 32;
    size -= 32;
  }
  while (size >= 8) {
    crc = __crc32cd(crc, load<std::uint64_t>(data));
    data += 8;
    size -= 8;
  }
  if (size >= 4) {
    crc = __crc32cw(crc, load<std::uint32_t>(data));
    data += 4;
    size -= 4;
  }
  if (size >= 2) {
    crc = __crc32ch(crc, load<std::uint16_t>(data));
    data += 2;
    size -= 2;
  }
  if (size)
    crc = __crc32cb(crc, std::to_integer<std::uint8_t>(*data));
  return ~crc;
}
#else
__attribute__((target("sse4.2"))) std::uint32_t
hardware(ByteSpan bytes, std::uint32_t seed) noexcept {
  std::uint64_t crc = static_cast<std::uint32_t>(~seed);
  auto *data = bytes.data();
  auto size = bytes.size();
  while (size >= 8) {
    crc = _mm_crc32_u64(crc, load<std::uint64_t>(data));
    data += 8;
    size -= 8;
  }
  auto tail = static_cast<std::uint32_t>(crc);
  if (size >= 4) {
    tail = _mm_crc32_u32(tail, load<std::uint32_t>(data));
    data += 4;
    size -= 4;
  }
  if (size >= 2) {
    tail = _mm_crc32_u16(tail, load<std::uint16_t>(data));
    data += 2;
    size -= 2;
  }
  if (size)
    tail = _mm_crc32_u8(tail, std::to_integer<std::uint8_t>(*data));
  return ~tail;
}
#endif
#endif
struct Backend {
  std::uint32_t (*function)(ByteSpan, std::uint32_t) noexcept;
  const char *name;
};
const Backend &backend() noexcept {
  static const Backend selected = []() -> Backend {
#if defined(__aarch64__) && !defined(RAF_PORTABLE_CRC)
    if (::getauxval(AT_HWCAP) & HWCAP_CRC32)
      return {hardware, "arm-crc32c"};
#elif defined(__x86_64__) && !defined(RAF_PORTABLE_CRC)
    if (__builtin_cpu_supports("sse4.2"))
      return {hardware, "x86-sse4.2"};
#endif
    return {crc32cPortable, "portable"};
  }();
  return selected;
}
} // namespace
std::uint32_t crc32c(ByteSpan bytes, std::uint32_t seed) noexcept {
  return backend().function(bytes, seed);
}
const char *crc32cBackend() noexcept { return backend().name; }
} // namespace raf
