/**
 * @file heartbeat.h
 * @brief HEARTBEAT_LOG_EVENT body: the keep-alive a source sends on an idle dump.
 *
 * Heartbeats exist only on the replication stream, never in a binlog file.
 * HEARTBEAT_LOG_EVENT (27) carries the file name as its whole body and the
 * position in the header's 32-bit next_position. HEARTBEAT_LOG_EVENT_V2 (41),
 * which MySQL 8.0.26 and later send, carries both in the body as
 * (type, length, value) fields of packed integers ending at type 0
 * (MySQL `libs/mysql/binlog/event/codecs/binary.cpp`, `Heartbeat::encode`):
 * type 1 is the file name (length = its bytes), type 2 the position as a
 * packed integer (read as one whatever the length says, as the source's
 * decoder does). Unknown field types are skipped by their length.
 */

#ifndef MYBINLOG_HEARTBEAT_H_
#define MYBINLOG_HEARTBEAT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Parsed heartbeat: the source's current binlog coordinates. */
struct HeartbeatEvent {
  std::string log_name;  ///< Binlog file the source is reading.
  /** Position in #log_name: the header's next_position (v1) or the body's field (v2). */
  uint64_t position = 0;
};

/**
 * @brief Parse a HEARTBEAT_LOG_EVENT (type 27) or HEARTBEAT_LOG_EVENT_V2 (type 41) body.
 *
 * A v2 body without a position field reports position 0, and one without a
 * file name an empty #HeartbeatEvent::log_name, as the source's decoder leaves
 * them.
 *
 * @param header Header of the event; selects the layout and, for v1, supplies the position.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated when @p body is null with a non-zero @p size, or a
 *   v2 field runs past the body or the body ends without the end mark;
 *   kInvalidMetadata for a v2 packed int 0xFB/0xFF; kUnsupported for any other
 *   type. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseHeartbeatEvent(const EventHeader& header,
                                                      const uint8_t* body, size_t size,
                                                      HeartbeatEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_HEARTBEAT_H_
