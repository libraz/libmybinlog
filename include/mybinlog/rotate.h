/**
 * @file rotate.h
 * @brief ROTATE_EVENT body.
 */

#ifndef MYBINLOG_ROTATE_H_
#define MYBINLOG_ROTATE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Parsed ROTATE_EVENT: where the next binlog file starts. */
struct RotateEvent {
  uint64_t position = 0;  ///< First event position in the next file.
  std::string next_file;  ///< Next file name, without any trailing NUL.
};

/**
 * @brief Parse a ROTATE_EVENT body: 8-byte LE position, then the file name.
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kTruncated when @p body is null or the position does not
 *   fit. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseRotateEvent(const EventHeader& header, const uint8_t* body,
                                                   size_t size, RotateEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_ROTATE_H_
