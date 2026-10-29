/**
 * @file xid.h
 * @brief XID_EVENT body: the commit of a transaction on a transactional engine.
 */

#ifndef MYBINLOG_XID_H_
#define MYBINLOG_XID_H_

#include <cstddef>
#include <cstdint>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Parsed XID_EVENT. */
struct XidEvent {
  uint64_t xid = 0;  ///< Server-internal transaction id, as written.
};

/**
 * @brief Parse an XID_EVENT body: an 8-byte little-endian xid.
 *
 * Bytes after the xid are ignored, as the servers do.
 *
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kTruncated when @p body is null or shorter than 8 bytes.
 *   Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseXidEvent(const EventHeader& header, const uint8_t* body,
                                                size_t size, XidEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_XID_H_
