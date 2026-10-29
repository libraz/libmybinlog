/**
 * @file crc32_internal.h
 * @brief zlib CRC32 over any length, inline for the per-event trailer check.
 */

#ifndef MYBINLOG_CRC32_INTERNAL_H_
#define MYBINLOG_CRC32_INTERNAL_H_

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace mybinlog {
namespace internal {

/** @brief UpdateCrc32(), feeding zlib at most its uInt length per call. */
inline uint32_t Crc32Update(uint32_t crc, const void* data, size_t size) {
  static_assert(sizeof(uInt) >= 4, "zlib uInt must be at least 32 bits");
  constexpr size_t kMaxChunk = static_cast<size_t>(std::numeric_limits<uInt>::max());
  const auto* bytes = static_cast<const Bytef*>(data);
  uLong running = static_cast<uLong>(crc);
  while (size > 0) {
    const size_t chunk = size > kMaxChunk ? kMaxChunk : size;
    running = crc32(running, bytes, static_cast<uInt>(chunk));
    bytes += chunk;
    size -= chunk;
  }
  return static_cast<uint32_t>(running);
}

}  // namespace internal
}  // namespace mybinlog

#endif  // MYBINLOG_CRC32_INTERNAL_H_
