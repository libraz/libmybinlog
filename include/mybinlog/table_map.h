/**
 * @file table_map.h
 * @brief TABLE_MAP_EVENT body: table identity, per-column types and metadata.
 *
 * Per-column metadata is read at the width upstream `read_field_metadata`
 * (`sql/rpl_utility.cc`) uses; a type whose width is unknown fails the event,
 * because a guess would misalign every later column. The optional metadata
 * TLVs that follow the NULL bitmap each index their own column subset:
 * SIGNEDNESS the numeric columns, (DEFAULT|COLUMN)_CHARSET the character
 * columns, ENUM_AND_SET_* and the label fields the ENUM/SET columns. Which
 * columns count as character columns differs by flavor (MariaDB counts
 * GEOMETRY), so parsing takes the flavor. Unknown TLV kinds are skipped by
 * their length.
 */

#ifndef MYBINLOG_TABLE_MAP_H_
#define MYBINLOG_TABLE_MAP_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/export.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Largest column count a TABLE_MAP may declare (MySQL's table column limit). */
inline constexpr size_t kMaxTableColumns = 4096;

/** @brief One column of a TABLE_MAP. */
struct TableMapColumn {
  /** Wire type code. ENUM and SET arrive as kString; see RealType(). */
  ColumnType type = ColumnType::kLong;
  /**
   * Type metadata as upstream `table_def` holds it: VARCHAR max length, BLOB
   * length-prefix width (1..4), FLOAT/DOUBLE pack length, temporal2 fsp,
   * NEWDECIMAL `precision << 8 | scale`, BIT `bytes << 8 | bits`, STRING
   * `real_type << 8 | length`. For kTypedArray, the element's metadata
   * (a VARCHAR element's length is 24-bit).
   */
  uint32_t meta = 0;
  /** Element type when `type == kTypedArray`; unused otherwise. */
  ColumnType element_type = ColumnType::kLong;
  /** Whether the column is nullable (the TABLE_MAP NULL bitmap). */
  bool nullable = false;
  /** From SIGNEDNESS; false when the field is absent (see TableMap::has_signedness). */
  bool is_unsigned = false;
  /** Whether a charset field covered this column. */
  bool has_charset = false;
  /** Collation id from (ENUM_AND_SET_)(DEFAULT|COLUMN)_CHARSET. */
  uint32_t charset = 0;
  /** From COLUMN_NAME; empty when absent. */
  std::string name;
  /** ENUM labels or SET members in declaration order; empty when absent. */
  std::vector<std::string> labels;
};

/** @brief Parsed TABLE_MAP_EVENT. */
struct TableMap {
  uint64_t table_id = 0;                ///< 48-bit table id the rows events refer to.
  std::string schema;                   ///< Database name.
  std::string table;                    ///< Table name.
  std::vector<TableMapColumn> columns;  ///< One entry per column, in ordinal order.
  bool has_signedness = false;          ///< A SIGNEDNESS field was present.
  bool has_column_names = false;        ///< A COLUMN_NAME field named every column.
  bool has_enum_labels = false;         ///< An ENUM_STR_VALUE field labelled every ENUM column.
  bool has_set_labels = false;          ///< A SET_STR_VALUE field labelled every SET column.
};

/**
 * @brief Whether a column type takes a bit in SIGNEDNESS.
 *
 * Upstream `has_signedess_information_type`: the integer types, YEAR,
 * FLOAT/DOUBLE and both DECIMALs. TYPED_ARRAY takes none, whatever its element.
 *
 * @param type Wire column type.
 * @return true for the types SIGNEDNESS indexes. Reentrant.
 */
[[nodiscard]] MYBINLOG_API bool HasSignednessBit(ColumnType type) noexcept;

/**
 * @brief The column's declared type.
 * @param column A column of a parsed TableMap.
 * @return kEnum or kSet for a kString column whose metadata byte 0 names one,
 *   the wire type otherwise. Reentrant.
 */
[[nodiscard]] MYBINLOG_API ColumnType RealType(const TableMapColumn& column) noexcept;

/**
 * @brief Parse a TABLE_MAP_EVENT body.
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param flavor Server flavor; decides which columns the charset fields index.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated when the body ends early (or @p body is null);
 *   kInvalidMetadata for a column count of 0 or above kMaxTableColumns, a
 *   metadata block that does not match the column types, a length-prefix
 *   width outside 1..4, a packed int 0xFB/0xFF, or an optional-metadata field
 *   inconsistent with the columns; kUnsupported for an unknown column type.
 *   Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseTableMapEvent(const EventHeader& header, const uint8_t* body,
                                                     size_t size, ServerFlavor flavor,
                                                     TableMap* out);

}  // namespace mybinlog

#endif  // MYBINLOG_TABLE_MAP_H_
