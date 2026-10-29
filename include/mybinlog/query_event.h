/**
 * @file query_event.h
 * @brief QUERY_EVENT body: fixed fields, the status variables the codec
 *   reports, the default schema and the statement.
 *
 * Status variables are walked with the per-code lengths of both servers
 * (MySQL codes 0-21, MariaDB 128-131 and the Q_DUMMY pad 255). Like the
 * servers, the walk stops at an unknown code: variables after it are not
 * reported, and `status_vars_complete` is false. The statement is not
 * classified.
 */

#ifndef MYBINLOG_QUERY_EVENT_H_
#define MYBINLOG_QUERY_EVENT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Q_CHARSET_CODE: the session's collation ids. */
struct QueryCharset {
  uint16_t client = 0;      ///< character_set_client
  uint16_t connection = 0;  ///< collation_connection
  uint16_t server = 0;      ///< collation_server
};

/** @brief Parsed QUERY_EVENT. */
struct QueryEvent {
  uint32_t thread_id = 0;     ///< Connection id of the session that ran the statement.
  uint32_t exec_time = 0;     ///< Execution time in seconds.
  uint16_t error_code = 0;    ///< Error the statement raised on the source; 0 for none.
  std::string schema;         ///< Default database; empty when none.
  std::string query;          ///< Statement text, bytes verbatim.
  bool has_sql_mode = false;  ///< Q_SQL_MODE_CODE was reached.
  uint64_t sql_mode = 0;      ///< Session sql_mode bits; meaningful when #has_sql_mode.
  bool has_charset = false;   ///< Q_CHARSET_CODE was reached.
  QueryCharset charset;       ///< Session collations; meaningful when #has_charset.
  /** The walk reached the end of the status variables (no unknown code). */
  bool status_vars_complete = false;
};

/**
 * @brief Parse a QUERY_EVENT body (13-byte post-header).
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated when the body ends early (or @p body is null);
 *   kInvalidMetadata when a known status variable runs past the status block
 *   or is malformed. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseQueryEvent(const EventHeader& header, const uint8_t* body,
                                                  size_t size, QueryEvent* out);

/** @brief How ParseQueryEvent() treats a status variable it cannot read. */
enum class StatusVarPolicy : uint8_t {
  kStrict,           ///< Fail with kInvalidMetadata, as ParseQueryEvent() does.
  kStopAtMalformed,  ///< End the walk there, like an unknown code; the statement is still read.
};

/**
 * @brief Parse a QUERY_EVENT body, choosing how an unreadable status variable is treated.
 *
 * The statement's position comes from the post-header lengths alone, so under
 * kStopAtMalformed a status variable that runs past the block or is malformed
 * ends the walk with #QueryEvent::status_vars_complete false, and the schema
 * and statement are read as usual.
 *
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @param policy Treatment of an unreadable status variable.
 * @return kOk; kTruncated when the body ends early (or @p body is null);
 *   kInvalidMetadata under kStrict as ParseQueryEvent() returns. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseQueryEventWithPolicy(const EventHeader& header,
                                                            const uint8_t* body, size_t size,
                                                            QueryEvent* out,
                                                            StatusVarPolicy policy);

}  // namespace mybinlog

#endif  // MYBINLOG_QUERY_EVENT_H_
