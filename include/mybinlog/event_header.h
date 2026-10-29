/**
 * @file event_header.h
 * @brief The 19-byte binlog v4 common event header and binlog file framing constants.
 */

#ifndef MYBINLOG_EVENT_HEADER_H_
#define MYBINLOG_EVENT_HEADER_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "mybinlog/event_types.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Size of the common header that opens every event. */
inline constexpr size_t kEventHeaderSize = 19;

/** @brief Size of the CRC32 trailer a checksummed event ends with. */
inline constexpr size_t kChecksumSize = 4;

/** @brief Magic number opening a binlog file (`\xfe` `bin`). */
inline constexpr std::array<uint8_t, 4> kBinlogMagic = {0xFE, 0x62, 0x69, 0x6E};

/** @brief Offset of the first event in a binlog file: the lowest position that names an event. */
inline constexpr uint64_t kBinlogMagicOffset = kBinlogMagic.size();

/** @brief Binlog v4 common header, all fields little-endian on the wire. */
struct EventHeader {
  uint32_t timestamp = 0;      ///< Seconds since the epoch when the event was written.
  uint8_t type_code = 0;       ///< An EventType value, or an unknown code.
  uint32_t server_id = 0;      ///< Server that originated the event.
  uint32_t event_length = 0;   ///< Whole event: header, body and any trailer.
  uint32_t next_position = 0;  ///< Position of the next event; 0 on artificial events.
  uint16_t flags = 0;          ///< `kLogEvent*Flag` bits.
};

/**
 * @brief Parse the common header at the start of an event.
 *
 * Only the 19 header bytes are read; @p size may cover the whole event.
 *
 * @param data First byte of the event.
 * @param size Bytes readable at @p data.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kTruncated when @p data is null or @p size is below
 *   kEventHeaderSize. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseEventHeader(const uint8_t* data, size_t size,
                                                   EventHeader* out);

}  // namespace mybinlog

#endif  // MYBINLOG_EVENT_HEADER_H_
