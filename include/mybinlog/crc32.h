/**
 * @file crc32.h
 * @brief CRC32 (zlib polynomial) as used by binlog event checksums.
 */

#ifndef MYBINLOG_CRC32_H_
#define MYBINLOG_CRC32_H_

#include <cstddef>
#include <cstdint>

#include "mybinlog/export.h"

namespace mybinlog {

/**
 * @brief Extend a running CRC32 over more bytes.
 *
 * Inputs longer than zlib's `uInt` are fed in chunks, so any `size_t` size
 * yields the true CRC.
 *
 * @param crc Previous value; 0 starts a new checksum.
 * @param data Bytes to add; may be null when @p size is 0.
 * @param size Bytes readable at @p data.
 * @return The updated CRC32. Reentrant.
 */
[[nodiscard]] MYBINLOG_API uint32_t UpdateCrc32(uint32_t crc, const void* data, size_t size);

/**
 * @brief CRC32 of a byte range.
 * @param data Bytes to checksum; may be null when @p size is 0.
 * @param size Bytes readable at @p data.
 * @return The CRC32 of the range. Reentrant.
 */
[[nodiscard]] inline uint32_t ComputeCrc32(const void* data, size_t size) {
  return UpdateCrc32(0, data, size);
}

}  // namespace mybinlog

#endif  // MYBINLOG_CRC32_H_
