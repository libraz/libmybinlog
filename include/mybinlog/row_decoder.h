/**
 * @file row_decoder.h
 * @brief WRITE/UPDATE/DELETE_ROWS event bodies (v1 and v2) decoded into Values.
 *
 * Each value's width is resolved by the same code that decodes it, so a
 * skipped column and a decoded one advance by the same rule. Only FULL row
 * images are decoded: a columns-present bitmap with a clear bit cannot be
 * mapped onto the TABLE_MAP columns without guessing, so it fails the event.
 * Every length is compared with the bytes that remain before it is used;
 * size and budget ceilings are a separate error (kLimitExceeded).
 */

#ifndef MYBINLOG_ROW_DECODER_H_
#define MYBINLOG_ROW_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {

/** @brief Default ceiling on the expansion of one MariaDB COMPRESSED value. */
inline constexpr size_t kMaxDecompressedColumnBytes = 64U * 1024U * 1024U;

/**
 * @brief Heap budget for everything one rows event materializes.
 *
 * Wire size bounds neither the row count nor the expansion of compressed
 * values, and a NULL costs one wire bit but a whole Value slot. Every row
 * image charges `column_count * per_value_overhead` before it is filled,
 * every value its payload bytes before they are copied (a compressed value is
 * clamped to what remains before inflating), and every row or UPDATE pair
 * `per_row_overhead` once decoded. `used` reads back what an event bought.
 */
struct DecodeBudget {
  size_t limit = kMaxDecompressedColumnBytes;  ///< Total bytes the event may charge.
  size_t used = 0;                             ///< Charged so far; never above #limit.

  /**
   * @brief Budget for an event body of @p body_bytes.
   *
   * At least @p expansion_ceiling, so a large compressed value still decodes;
   * above that it follows the body size, which bounds every uncompressed value.
   */
  static DecodeBudget ForEventBody(size_t body_bytes,
                                   size_t expansion_ceiling = kMaxDecompressedColumnBytes) {
    DecodeBudget budget;
    budget.limit = body_bytes > expansion_ceiling ? body_bytes : expansion_ceiling;
    return budget;
  }

  /** @brief Bytes still available. */
  size_t Remaining() const { return limit - used; }

  /** @brief Charge @p bytes; false (charging nothing) when they do not fit. */
  bool Charge(size_t bytes) {
    if (bytes > Remaining()) return false;
    used += bytes;
    return true;
  }
};

/** @brief What DecodeRowsEvent() materializes and how much it may spend. */
struct DecodeOptions {
  /**
   * Columns to decode, by TABLE_MAP ordinal; empty decodes every column. An
   * ordinal that is false or past the end is measured and skipped without
   * materializing: its metadata is still validated (the width depends on it)
   * but value-level checks are not run, and it comes back as Value::Kind::kNull
   * whether or not the row held NULL there.
   */
  std::vector<bool> columns;
  /** Ceiling on one decoded value's payload bytes; 0 = none. */
  size_t max_value_bytes = 0;
  /** Ceiling on one COMPRESSED value's expansion; 0 = none. */
  size_t max_decompressed_bytes = kMaxDecompressedColumnBytes;
  /** Event-wide budget, charged as DecodeBudget describes; nullptr = none. */
  DecodeBudget* budget = nullptr;
  /** Bytes charged per Value slot of every row image. */
  size_t per_value_overhead = 0;
  /** Bytes charged per decoded row (per pair for UPDATE). */
  size_t per_row_overhead = 0;
};

/** @brief One row image, a Value per TABLE_MAP column in ordinal order. */
using Row = std::vector<Value>;

/** @brief Decoded rows event: #rows for WRITE and DELETE, #updates (before, after) for UPDATE. */
struct RowsEventData {
  std::vector<Row> rows;
  std::vector<std::pair<Row, Row>> updates;
};

/**
 * @brief Decode a WRITE/UPDATE/DELETE_ROWS body (types 23-25 and 30-32).
 *
 * A column whose TableMap entry has no charset decodes character data as
 * kBytes; without SIGNEDNESS (TableMap::has_signedness false) integers decode
 * as signed.
 *
 * @param header Header of the event; its type code selects v1/v2 and the shape.
 * @param body Bytes after the common header.
 * @param len Body length, checksum trailer excluded.
 * @param table The TABLE_MAP the event's table id refers to.
 * @param options Column selection and limits.
 * @param[out] out Filled on success; untouched on failure.
 * @return kOk; kTruncated when the body or a value ends early (or @p body is
 *   null); kInvalidMetadata for a column count of 0, above kMaxTableColumns or
 *   unequal to the table's, a v2 extra-data length below 2, a packed int
 *   0xFB/0xFF, or column metadata no server writes (length-prefix width
 *   outside 1..4, fsp above 6, DECIMAL precision outside 1..65 or scale above
 *   min(precision, 38), ENUM size outside 1..2, SET outside 1..8, BIT wider
 *   than 64); kMalformedValue for a value no server writes (DATE/DATETIME
 *   month > 12, day > 31, hour > 23, minute or second > 59, year > 9999, a
 *   negative DATETIME, TIME hour > 838, microsecond > 999999, a zero TIMESTAMP
 *   with a fraction, a DECIMAL digit group past its digit count, a COMPRESSED
 *   header of method 0 with low bits set, a compressed value with trailing
 *   input, an original length of 0 or above the column's maximum, or a corrupt
 *   stream); kUnsupported for a type code that is not a rows event, a partial
 *   row image, a column type the codec does not decode, or an unknown
 *   compression method; kLimitExceeded when a value exceeds
 *   DecodeOptions::max_value_bytes or max_decompressed_bytes or the budget
 *   runs out.
 */
Status DecodeRowsEvent(const EventHeader& header, const uint8_t* body, size_t len,
                       const TableMap& table, const DecodeOptions& options, RowsEventData* out);

}  // namespace mybinlog

#endif  // MYBINLOG_ROW_DECODER_H_
