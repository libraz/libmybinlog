/**
 * @file row_decoder.h
 * @brief WRITE/UPDATE/DELETE_ROWS and PARTIAL_UPDATE_ROWS bodies decoded into Values.
 *
 * Each value's width is resolved by the same code that decodes it, so a
 * skipped column and a decoded one advance by the same rule. FULL, MINIMAL and
 * NOBLOB row images all decode: the columns-present bitmap of each image says
 * which columns it carries, its NULL bitmap has one bit per carried column, and
 * a column it leaves out comes back as Value::Kind::kAbsent. Every length is
 * compared with the bytes that remain before it is used; size and budget
 * ceilings are a separate error (kLimitExceeded).
 */

#ifndef MYBINLOG_ROW_DECODER_H_
#define MYBINLOG_ROW_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
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
 *
 * Thread safety: a budget is mutated by the decode it is passed to, so it must
 * not be shared between concurrent calls.
 */
struct DecodeBudget {
  size_t limit = kMaxDecompressedColumnBytes;  ///< Total bytes the event may charge.
  size_t used = 0;                             ///< Charged so far; never above #limit.

  /**
   * @brief Budget for an event body of @p body_bytes.
   *
   * At least @p expansion_ceiling, so a large compressed value still decodes;
   * above that it follows the body size, which bounds every uncompressed value.
   *
   * @param body_bytes Size of the rows event body.
   * @param expansion_ceiling Floor of the limit.
   * @return A budget with nothing charged.
   */
  [[nodiscard]] static DecodeBudget ForEventBody(
      size_t body_bytes, size_t expansion_ceiling = kMaxDecompressedColumnBytes) {
    DecodeBudget budget;
    budget.limit = body_bytes > expansion_ceiling ? body_bytes : expansion_ceiling;
    return budget;
  }

  /** @return Bytes still available. */
  [[nodiscard]] size_t Remaining() const { return limit - used; }

  /**
   * @brief Charge bytes against the budget.
   * @param bytes Bytes to charge.
   * @return true when charged; false, charging nothing, when they do not fit.
   */
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
   * whether or not the row held NULL there or left it out of the image.
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

/**
 * @brief One row image, a Value per TABLE_MAP column in ordinal order.
 *
 * A column the image does not carry (MINIMAL or NOBLOB) is Value::Kind::kAbsent.
 */
using Row = std::vector<Value>;

/**
 * @brief Decoded rows event.
 *
 * The partition fields come from the v2 extra row info, which MySQL writes for
 * a row change in a partitioned table (`enum_extra_row_info_typecode::PART`);
 * the NDB extra row info is skipped and not exposed.
 */
struct RowsEvent {
  std::vector<Row> rows;                     ///< WRITE and DELETE: one image per row.
  std::vector<std::pair<Row, Row>> updates;  ///< UPDATE and PARTIAL_UPDATE: (before, after).
  bool has_partition_id = false;             ///< The extra row info named a partition.
  uint16_t partition_id = 0;                 ///< Partition the rows are in (after an UPDATE).
  /** UPDATE and PARTIAL_UPDATE: partition the rows were in before the update. */
  uint16_t source_partition_id = 0;
};

/**
 * @brief Decode a WRITE/UPDATE/DELETE_ROWS body (types 23-25 and 30-32) or a
 *   PARTIAL_UPDATE_ROWS body (type 39).
 *
 * A column whose TableMap entry has no charset decodes character data as
 * kBytes; without SIGNEDNESS (TableMap::has_signedness false) integers decode
 * as signed. A MySQL JSON column is kJson (MariaDB declares JSON as LONGTEXT,
 * which stays kText).
 *
 * PARTIAL_UPDATE_ROWS lays out like UPDATE_ROWS except that every after image
 * opens with a packed `binlog_row_value_options` (0, or 1 for PARTIAL_JSON) and,
 * when it is 1, a bitmap with one bit per JSON column of the table (whether or
 * not the image carries it). A JSON column whose bit is set holds a
 * `Json_diff_vector` instead of a document and decodes as kJsonDiff; its
 * #Value::bytes is the vector exactly as `Json_diff_vector::write_binary`
 * writes it, the 4-byte little-endian length included. Every other column, and
 * every row whose options are 0, decodes as in UPDATE_ROWS.
 *
 * @param header Header of the event; its type code selects v1/v2 and the shape.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param table The TABLE_MAP the event's table id refers to; only read during the call.
 * @param options Column selection and limits; `options.budget`, when set, is
 *   charged even when the call fails.
 * @param[out] out Filled on success; untouched on failure.
 * @return kOk; kTruncated when the body or a value ends early (or @p body is
 *   null); kInvalidMetadata for a column count of 0, above kMaxTableColumns or
 *   unequal to the table's, a v2 extra-data length below 2 or an extra row
 *   info entry that overruns it, a packed int 0xFB/0xFF, a value-options
 *   field above 1, a body whose column bitmaps select no column but that still
 *   holds bytes, or column metadata no server writes (length-prefix width
 *   outside 1..4, fsp above 6, DECIMAL precision outside 1..65 or scale above
 *   min(precision, 38), ENUM size outside 1..2, SET outside 1..8, BIT wider
 *   than 64); kMalformedValue for a value no server writes (DATE/DATETIME
 *   month > 12, day > 31, hour > 23, minute or second > 59, year > 9999, a
 *   negative DATETIME, TIME hour > 838, microsecond > 999999, a zero TIMESTAMP
 *   with a fraction, a DECIMAL digit group past its digit count, a COMPRESSED
 *   header of method 0 with low bits set, a compressed value with trailing
 *   input, an original length of 0 or above the column's maximum, or a corrupt
 *   stream); kUnsupported for a type code that is not a rows event, a column type the codec does
 * not decode, or an unknown compression method; kLimitExceeded when a value exceeds
 *   DecodeOptions::max_value_bytes or max_decompressed_bytes or the budget
 *   runs out. Reentrant; concurrent calls must not share a DecodeBudget.
 */
[[nodiscard]] MYBINLOG_API Status DecodeRowsEvent(const EventHeader& header, const uint8_t* body,
                                                  size_t size, const TableMap& table,
                                                  const DecodeOptions& options, RowsEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_ROW_DECODER_H_
