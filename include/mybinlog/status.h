/**
 * @file status.h
 * @brief Result code returned by every fallible libmybinlog function.
 *
 * Functions report failure through the returned Status and deliver results
 * through a trailing out parameter, which they leave untouched on failure
 * unless their documentation says otherwise. No function throws.
 */

#ifndef MYBINLOG_STATUS_H_
#define MYBINLOG_STATUS_H_

#include <cstdint>

#include "mybinlog/export.h"

namespace mybinlog {

/** @brief Outcome of a libmybinlog call. */
enum class Status : uint8_t {
  kOk,                ///< Success.
  kTruncated,         ///< Input ended before the structure was complete.
  kInvalidMetadata,   ///< Column or table metadata is inconsistent or out of range.
  kMalformedValue,    ///< A value's bytes are not a valid encoding for its type.
  kUnsupported,       ///< A well-formed type or event the library does not decode.
  kChecksumMismatch,  ///< CRC32 mismatch, or a frame too short to carry its trailer.
  kLimitExceeded,     ///< A configured size or budget limit was exceeded.
};

/**
 * @brief Stable identifier of a status, for logs and messages.
 * @param status Any value, including one outside the enumerators.
 * @return Static string such as "kTruncated" ("kUnknown" outside the
 *   enumerators); never null. Reentrant.
 */
[[nodiscard]] MYBINLOG_API const char* StatusName(Status status) noexcept;

}  // namespace mybinlog

#endif  // MYBINLOG_STATUS_H_
