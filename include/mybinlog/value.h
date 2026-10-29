/**
 * @file value.h
 * @brief One decoded column value, structured and unrendered.
 *
 * The codec returns what the wire says and leaves presentation to the caller:
 * temporals stay as fields (zero and partial-zero dates included, never NULL),
 * TIMESTAMP stays as epoch seconds, DECIMAL is its exact decimal text (`-0.00`
 * kept), ENUM/SET are an ordinal/bitmask whose labels live in TableMap, and
 * JSON, GEOMETRY, VECTOR and TYPED_ARRAY are the raw bytes of their payload.
 */

#ifndef MYBINLOG_VALUE_H_
#define MYBINLOG_VALUE_H_

#include <cstdint>
#include <string>

#include "mybinlog/event_types.h"

namespace mybinlog {

/** @brief Calendar and clock fields of a DATE, DATETIME or TIME value. */
struct Temporal {
  bool negative = false;     ///< TIME only; a DATE or DATETIME is never negative.
  uint16_t year = 0;         ///< 0..9999.
  uint8_t month = 0;         ///< 0..12; 0 in a zero or partial-zero date.
  uint8_t day = 0;           ///< 0..31; 0 in a zero or partial-zero date.
  uint16_t hour = 0;         ///< 0..23, or 0..838 for TIME.
  uint8_t minute = 0;        ///< 0..59.
  uint8_t second = 0;        ///< 0..59.
  uint32_t microsecond = 0;  ///< 0..999999.
  uint8_t fsp = 0;           ///< Declared fractional-second precision, 0..6.
};

/**
 * @brief A decoded column value.
 *
 * Only the members named for #kind are meaningful; the rest keep their
 * defaults.
 */
struct Value {
  /** @brief Shape of the value. */
  enum class Kind : uint8_t {
    kNull,       ///< SQL NULL, or a column DecodeOptions::columns did not select.
    kInt,        ///< Signed integer in #i.
    kUInt,       ///< Unsigned integer in #u.
    kDouble,     ///< FLOAT (widened) or DOUBLE in #d; NaN and infinities pass through.
    kDecimal,    ///< DECIMAL as decimal text in #bytes, e.g. "-12.50".
    kDate,       ///< DATE in #t.
    kDateTime,   ///< DATETIME in #t.
    kTime,       ///< TIME in #t.
    kTimestamp,  ///< TIMESTAMP in #epoch_seconds, #microsecond and #fsp; wire 0 stays 0.
    kYear,       ///< YEAR in #u: 0 for the zero year, else 1901..2155.
    kEnum,       ///< ENUM ordinal in #u (0 is the empty value, labels are 1-based).
    kSet,        ///< SET member bitmask in #u (bit n is label n).
    kBit,        ///< BIT value in #u.
    kText,       ///< Character data in a non-binary collation, in #bytes.
    kBytes,      ///< Binary data, or character data whose collation is unknown, in #bytes.
  };

  Kind kind = Kind::kNull;
  /** Column type: RealType() of the column, so ENUM and SET read kEnum / kSet. */
  ColumnType type = ColumnType::kLong;
  int64_t i = 0;
  uint64_t u = 0;
  double d = 0.0;
  Temporal t;
  int64_t epoch_seconds = 0;
  uint32_t microsecond = 0;
  uint8_t fsp = 0;
  std::string bytes;
};

/** @brief Stable identifier of @p kind (e.g. "kDecimal"), for logs and messages. */
const char* ValueKindName(Value::Kind kind) noexcept;

}  // namespace mybinlog

#endif  // MYBINLOG_VALUE_H_
