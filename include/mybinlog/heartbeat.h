/**
 * @file heartbeat.h
 * @brief HEARTBEAT_LOG_EVENT body: the keep-alive a source sends on an idle dump.
 *
 * Heartbeats exist only on the replication stream, never in a binlog file.
 * The v2 form (type 41), which MySQL uses for positions past 4 GiB, is not
 * decoded.
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
  std::string log_name;   ///< Binlog file the source is reading.
  uint64_t position = 0;  ///< Position in #log_name, from the header's next_position.
};

/**
 * @brief Parse a HEARTBEAT_LOG_EVENT (type 27) body: the binlog file name.
 * @param header Header of the event; selects the layout and supplies the position.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated when @p body is null with a non-zero @p size;
 *   kUnsupported for HEARTBEAT_LOG_EVENT_V2 (41) or any other type. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseHeartbeatEvent(const EventHeader& header,
                                                      const uint8_t* body, size_t size,
                                                      HeartbeatEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_HEARTBEAT_H_
