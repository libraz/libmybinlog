/**
 * @file rotate.h
 * @brief ROTATE_EVENT body.
 */

#ifndef MYBINLOG_ROTATE_H_
#define MYBINLOG_ROTATE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Where the next binlog file starts. */
struct RotateEvent {
  uint64_t position = 0;  ///< First event position in the next file.
  std::string next_file;  ///< Next file name, without any trailing NUL.
};

/**
 * @brief Parse a ROTATE_EVENT body: 8-byte LE position, then the file name.
 * @param body Bytes after the common header.
 * @param len Body length, trailer excluded.
 * @return kOk, or kTruncated when the position does not fit.
 */
Status ParseRotateEvent(const uint8_t* body, size_t len, RotateEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_ROTATE_H_
