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
  uint32_t thread_id = 0;
  uint32_t exec_time = 0;  ///< Seconds.
  uint16_t error_code = 0;
  std::string schema;  ///< Default database; empty when none.
  std::string query;
  bool has_sql_mode = false;  ///< Q_SQL_MODE_CODE was reached.
  uint64_t sql_mode = 0;
  bool has_charset = false;  ///< Q_CHARSET_CODE was reached.
  QueryCharset charset;
  /** The walk reached the end of the status variables (no unknown code). */
  bool status_vars_complete = false;
};

/**
 * @brief Parse a QUERY_EVENT body (13-byte post-header).
 * @param body Bytes after the common header.
 * @param len Body length, checksum trailer excluded.
 * @param[out] out Filled on success; untouched on failure.
 * @return kOk; kTruncated when the body ends early (or @p body is null);
 *   kInvalidMetadata when a known status variable runs past the status block
 *   or is malformed.
 */
Status ParseQueryEvent(const uint8_t* body, size_t len, QueryEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_QUERY_EVENT_H_
